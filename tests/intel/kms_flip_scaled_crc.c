/*
 * Copyright © 2020 Intel Corporation
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
 * TEST: kms flip scaled crc
 * Category: Display
 * Description: Test flipping between scaled/nonscaled framebuffers
 * Driver requirement: i915, xe
 * Mega feature: General Display Features
 */

#include "igt.h"

/**
 * SUBTEST: flip-64bpp-ytile-to-32bpp-ytilegen12rcccs-upscaling
 * Description: Flip from 64bpp non scaled fb to 32bpp upscaled fb to stress CD
 *              clock programming
 *
 * SUBTEST: flip-64bpp-ytile-to-32bpp-ytilercccs-downscaling
 * Description: Flip from 64bpp non scaled fb to 32bpp downscaled fb to stress CD
 *              clock programming
 *
 * SUBTEST: flip-32bpp-linear-to-64bpp-linear-%s
 * Description: Flip from 32bpp non scaled fb to 64bpp %arg[1] fb to stress CD
 *              clock programming
 *
 * SUBTEST: flip-32bpp-4tile-to-32bpp-4tiledg2rcccs-%s
 * Description: Flip from 32bpp non scaled fb to 32bpp %arg[1] fb to stress CD
 *              clock programming
 *
 * SUBTEST: flip-32bpp-4tile-to-64bpp-4tile-%s
 * Description: Flip from 32bpp non scaled fb to 64bpp %arg[1] fb to stress CD
 *              clock programming
 *
 * SUBTEST: flip-32bpp-xtile-to-64bpp-xtile-%s
 * Description: Flip from 32bpp non scaled fb to 64bpp %arg[1] fb to stress CD
 *              clock programming
 *
 * SUBTEST: flip-32bpp-yftile-to-32bpp-yftileccs-%s
 * Description: Flip from 32bpp non scaled fb to 32bpp %arg[1] fb to stress CD
 *              clock programming
 *
 * SUBTEST: flip-32bpp-yftile-to-64bpp-yftile-%s
 * Description: Flip from 32bpp non scaled fb to 64bpp %arg[1] fb to stress CD
 *              clock programming
 *
 * SUBTEST: flip-32bpp-yftileccs-to-64bpp-yftile-%s
 * Description: Flip from 32bpp non scaled fb to 64bpp %arg[1] fb to stress CD
 *              clock programming
 *
 * SUBTEST: flip-32bpp-ytile-to-32bpp-ytileccs-%s
 * Description: Flip from 32bpp non scaled fb to 32bpp %arg[1] fb to stress CD
 *              clock programming
 *
 * SUBTEST: flip-32bpp-ytile-to-32bpp-ytilegen12rcccs-%s
 * Description: Flip from 32bpp non scaled fb to 32bpp %arg[1] fb to stress CD
 *              clock programming
 *
 * SUBTEST: flip-32bpp-ytile-to-64bpp-ytile-%s
 * Description: Flip from 32bpp non scaled fb to 64bpp %arg[1] fb to stress CD
 *              clock programming
 *
 * SUBTEST: flip-32bpp-ytileccs-to-64bpp-ytile-%s
 * Description: Flip from 32bpp non scaled fb to 64bpp %arg[1] fb to stress CD
 *              clock programming
 *
 * SUBTEST: flip-64bpp-4tile-to-32bpp-4tiledg2rcccs-%s
 * Description: Flip from 64bpp non scaled fb to 32bpp %arg[1] fb to stress CD
 *              clock programming
 *
 * arg[1]:
 *
 * @downscaling:        Downscaled
 * @upscaling:          Upscaled
 */

/**
 * SUBTEST: flip-64bpp-4tile-to-%dbpp-4tile-%s
 * Description: Flip from 64bpp non scaled fb to %arg[1]bpp %arg[2] fb to stress
 *              CD clock programming
 *
 * SUBTEST: flip-64bpp-xtile-to-%dbpp-xtile-%s
 * Description: Flip from 64bpp non scaled fb to %arg[1]bpp %arg[2] fb to stress
 *              CD clock programming
 *
 * SUBTEST: flip-64bpp-yftile-to-%dbpp-yftile-%s
 * Description: Flip from 64bpp non scaled fb to %arg[1]bpp %arg[2] fb to stress
 *              CD clock programming
 *
 * SUBTEST: flip-64bpp-ytile-to-%dbpp-ytile-%s
 * Description: Flip from 64bpp non scaled fb to %arg[1]bpp %arg[2] fb to stress
 *              CD clock programming
 *
 * SUBTEST: flip-64bpp-linear-to-%dbpp-linear-%s
 * Description: Flip from 64bpp non scaled fb to %arg[1]bpp %arg[2] fb to stress
 *              CD clock programming
 *
 * arg[1].values: 16, 32
 * arg[2]:
 *
 * @downscaling:        Downscaled
 * @upscaling:          Upscaled
 */
/**
 * SUBTEST: flip-32bpp-linear-to-32bpp-linear-reflect-x
 * Description: Flip from 32bpp non scaled fb to 32bpp non scaled x mirrored fb
 *
 * SUBTEST: flip-32bpp-yuv-linear-to-32bpp-yuv-linear-reflect-x
 * Description: Flip from 32bpp xyuv non scaled fb to 32bpp xyuv non scaled
 *              x mirrored fb
 *
 * SUBTEST: flip-NV12-linear-to-NV12-linear-reflect-x
 * Description: Flip from NV12 non scaled fb to NV12 non scaled x mirrored fb
 *
 * SUBTEST: flip-P016-linear-to-P016-linear-reflect-x
 * Description: Flip from P016 non scaled fb to P016 non scaled x mirrored fb
 */

IGT_TEST_DESCRIPTION("Test flipping between scaled/nonscaled framebuffers");

typedef struct {
	int drm_fd;
	igt_display_t display;
	igt_output_t *output;
	uint32_t gen;
	struct igt_fb small_fb;
	struct igt_fb big_fb;
	igt_pipe_crc_t *pipe_crc;
	uint32_t attemptmodewidth;
	uint32_t attemptmodeheight;
	drmModeModeInfo mode;
} data_t;

const struct {
	const char * const name;
	const char * const describe;
	const uint64_t firstmodifier;
	const uint32_t firstformat;
	const uint64_t secondmodifier;
	const uint32_t secondformat;
	const double firstmultiplier;
	const double secondmultiplier;
	const igt_rotation_t firstrotation;
	const igt_rotation_t secondrotation;
} flip_scenario_test[] = {
	{
		"flip-32bpp-ytile-to-64bpp-ytile-downscaling",
		"Flip from 32bpp non scaled fb to 64bpp downscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_Y_TILED, DRM_FORMAT_XRGB8888,
		I915_FORMAT_MOD_Y_TILED, DRM_FORMAT_XRGB16161616F,
		1.0,
		2.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-32bpp-yftile-to-64bpp-yftile-downscaling",
		"Flip from 32bpp non scaled fb to 64bpp downscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_Yf_TILED, DRM_FORMAT_XRGB8888,
		I915_FORMAT_MOD_Yf_TILED, DRM_FORMAT_XRGB16161616F,
		1.0,
		2.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-32bpp-xtile-to-64bpp-xtile-downscaling",
		"Flip from 32bpp non scaled fb to 64bpp downscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_X_TILED, DRM_FORMAT_XRGB8888,
		I915_FORMAT_MOD_X_TILED, DRM_FORMAT_XRGB16161616F,
		1.0,
		2.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-32bpp-4tile-to-64bpp-4tile-downscaling",
		"Flip from 32bpp non scaled fb to 64bpp downscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_4_TILED, DRM_FORMAT_XRGB8888,
		I915_FORMAT_MOD_4_TILED, DRM_FORMAT_XRGB16161616F,
		1.0,
		2.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-32bpp-linear-to-64bpp-linear-downscaling",
		"Flip from 32bpp non scaled fb to 64bpp downscaled fb to stress CD clock programming",
		DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_XRGB8888,
		DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_XRGB16161616F,
		1.0,
		2.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-64bpp-ytile-to-32bpp-ytile-downscaling",
		"Flip from 64bpp non scaled fb to 32bpp downscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_Y_TILED, DRM_FORMAT_XRGB16161616F,
		I915_FORMAT_MOD_Y_TILED, DRM_FORMAT_XRGB8888,
		1.0,
		2.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-64bpp-yftile-to-32bpp-yftile-downscaling",
		"Flip from 64bpp non scaled fb to 32bpp downscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_Yf_TILED, DRM_FORMAT_XRGB16161616F,
		I915_FORMAT_MOD_Yf_TILED, DRM_FORMAT_XRGB8888,
		1.0,
		2.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-64bpp-xtile-to-32bpp-xtile-downscaling",
		"Flip from 64bpp non scaled fb to 32bpp downscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_X_TILED, DRM_FORMAT_XRGB16161616F,
		I915_FORMAT_MOD_X_TILED, DRM_FORMAT_XRGB8888,
		1.0,
		2.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-64bpp-4tile-to-32bpp-4tile-downscaling",
		"Flip from 64bpp non scaled fb to 32bpp downscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_4_TILED, DRM_FORMAT_XRGB16161616F,
		I915_FORMAT_MOD_4_TILED, DRM_FORMAT_XRGB8888,
		1.0,
		2.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-64bpp-linear-to-32bpp-linear-downscaling",
		"Flip from 64bpp non scaled fb to 32bpp downscaled fb to stress CD clock programming",
		DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_XRGB16161616F,
		DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_XRGB8888,
		1.0,
		2.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-64bpp-ytile-to-16bpp-ytile-downscaling",
		"Flip from 64bpp non scaled fb to 16bpp downscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_Y_TILED, DRM_FORMAT_XRGB16161616F,
		I915_FORMAT_MOD_Y_TILED, DRM_FORMAT_RGB565,
		1.0,
		2.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-64bpp-yftile-to-16bpp-yftile-downscaling",
		"Flip from 64bpp non scaled fb to 16bpp downscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_Yf_TILED, DRM_FORMAT_XRGB16161616F,
		I915_FORMAT_MOD_Yf_TILED, DRM_FORMAT_RGB565,
		1.0,
		2.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-64bpp-xtile-to-16bpp-xtile-downscaling",
		"Flip from 64bpp non scaled fb to 16bpp downscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_X_TILED, DRM_FORMAT_XRGB16161616F,
		I915_FORMAT_MOD_X_TILED, DRM_FORMAT_RGB565,
		1.0,
		2.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-64bpp-4tile-to-16bpp-4tile-downscaling",
		"Flip from 64bpp non scaled fb to 16bpp downscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_4_TILED, DRM_FORMAT_XRGB16161616F,
		I915_FORMAT_MOD_4_TILED, DRM_FORMAT_RGB565,
		1.0,
		2.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-64bpp-linear-to-16bpp-linear-downscaling",
		"Flip from 64bpp non scaled fb to 16bpp downscaled fb to stress CD clock programming",
		DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_XRGB16161616F,
		DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_RGB565,
		1.0,
		2.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-32bpp-ytileccs-to-64bpp-ytile-downscaling",
		"Flip from 32bpp non scaled fb to 64bpp downscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_Y_TILED_CCS, DRM_FORMAT_XRGB8888,
		I915_FORMAT_MOD_Y_TILED, DRM_FORMAT_XRGB16161616F,
		1.0,
		2.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-32bpp-yftileccs-to-64bpp-yftile-downscaling",
		"Flip from 32bpp non scaled fb to 64bpp downscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_Yf_TILED_CCS, DRM_FORMAT_XRGB8888,
		I915_FORMAT_MOD_Yf_TILED, DRM_FORMAT_XRGB16161616F,
		1.0,
		2.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-32bpp-ytile-to-32bpp-ytilegen12rcccs-downscaling",
		"Flip from 32bpp non scaled fb to 32bpp downscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_Y_TILED, DRM_FORMAT_XRGB8888,
		I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS, DRM_FORMAT_XRGB8888,
		1.0,
		2.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-32bpp-4tile-to-32bpp-4tiledg2rcccs-downscaling",
		"Flip from 32bpp non scaled fb to 32bpp downscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_4_TILED, DRM_FORMAT_XRGB8888,
		I915_FORMAT_MOD_4_TILED_DG2_RC_CCS, DRM_FORMAT_XRGB8888,
		1.0,
		2.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-32bpp-ytile-to-32bpp-ytileccs-downscaling",
		"Flip from 32bpp non scaled fb to 32bpp downscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_Y_TILED, DRM_FORMAT_XRGB8888,
		I915_FORMAT_MOD_Y_TILED_CCS, DRM_FORMAT_XRGB8888,
		1.0,
		2.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-32bpp-yftile-to-32bpp-yftileccs-downscaling",
		"Flip from 32bpp non scaled fb to 32bpp downscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_Yf_TILED, DRM_FORMAT_XRGB8888,
		I915_FORMAT_MOD_Yf_TILED_CCS, DRM_FORMAT_XRGB8888,
		1.0,
		2.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-64bpp-ytile-to-32bpp-ytilercccs-downscaling",
		"Flip from 64bpp non scaled fb to 32bpp downscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_Y_TILED, DRM_FORMAT_XRGB16161616F,
		I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS, DRM_FORMAT_XRGB8888,
		1.0,
		2.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-64bpp-4tile-to-32bpp-4tiledg2rcccs-downscaling",
		"Flip from 64bpp non scaled fb to 32bpp downscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_4_TILED, DRM_FORMAT_XRGB16161616F,
		I915_FORMAT_MOD_4_TILED_DG2_RC_CCS, DRM_FORMAT_XRGB8888,
		1.0,
		2.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-32bpp-ytile-to-64bpp-ytile-upscaling",
		"Flip from 32bpp non scaled fb to 64bpp upscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_Y_TILED, DRM_FORMAT_XRGB8888,
		I915_FORMAT_MOD_Y_TILED, DRM_FORMAT_XRGB16161616F,
		0.5,
		1.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-32bpp-yftile-to-64bpp-yftile-upscaling",
		"Flip from 32bpp non scaled fb to 64bpp upscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_Yf_TILED, DRM_FORMAT_XRGB8888,
		I915_FORMAT_MOD_Yf_TILED, DRM_FORMAT_XRGB16161616F,
		0.5,
		1.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-32bpp-xtile-to-64bpp-xtile-upscaling",
		"Flip from 32bpp non scaled fb to 64bpp upscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_X_TILED, DRM_FORMAT_XRGB8888,
		I915_FORMAT_MOD_X_TILED, DRM_FORMAT_XRGB16161616F,
		0.5,
		1.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-32bpp-4tile-to-64bpp-4tile-upscaling",
		"Flip from 32bpp non scaled fb to 64bpp upscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_4_TILED, DRM_FORMAT_XRGB8888,
		I915_FORMAT_MOD_4_TILED, DRM_FORMAT_XRGB16161616F,
		0.5,
		1.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-32bpp-linear-to-64bpp-linear-upscaling",
		"Flip from 32bpp non scaled fb to 64bpp upscaled fb to stress CD clock programming",
		DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_XRGB8888,
		DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_XRGB16161616F,
		0.5,
		1.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-64bpp-ytile-to-32bpp-ytile-upscaling",
		"Flip from 64bpp non scaled fb to 32bpp upscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_Y_TILED, DRM_FORMAT_XRGB16161616F,
		I915_FORMAT_MOD_Y_TILED, DRM_FORMAT_XRGB8888,
		0.5,
		1.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-64bpp-yftile-to-32bpp-yftile-upscaling",
		"Flip from 64bpp non scaled fb to 32bpp upscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_Yf_TILED, DRM_FORMAT_XRGB16161616F,
		I915_FORMAT_MOD_Yf_TILED, DRM_FORMAT_XRGB8888,
		0.5,
		1.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-64bpp-xtile-to-32bpp-xtile-upscaling",
		"Flip from 64bpp non scaled fb to 32bpp upscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_X_TILED, DRM_FORMAT_XRGB16161616F,
		I915_FORMAT_MOD_X_TILED, DRM_FORMAT_XRGB8888,
		0.5,
		1.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-64bpp-4tile-to-32bpp-4tile-upscaling",
		"Flip from 64bpp non scaled fb to 32bpp upscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_4_TILED, DRM_FORMAT_XRGB16161616F,
		I915_FORMAT_MOD_4_TILED, DRM_FORMAT_XRGB8888,
		0.5,
		1.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-64bpp-linear-to-32bpp-linear-upscaling",
		"Flip from 64bpp non scaled fb to 32bpp upscaled fb to stress CD clock programming",
		DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_XRGB16161616F,
		DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_XRGB8888,
		0.5,
		1.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-64bpp-ytile-to-16bpp-ytile-upscaling",
		"Flip from 64bpp non scaled fb to 16bpp upscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_Y_TILED, DRM_FORMAT_XRGB16161616F,
		I915_FORMAT_MOD_Y_TILED, DRM_FORMAT_RGB565,
		0.5,
		1.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-64bpp-yftile-to-16bpp-yftile-upscaling",
		"Flip from 64bpp non scaled fb to 16bpp upscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_Yf_TILED, DRM_FORMAT_XRGB16161616F,
		I915_FORMAT_MOD_Yf_TILED, DRM_FORMAT_RGB565,
		0.5,
		1.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-64bpp-xtile-to-16bpp-xtile-upscaling",
		"Flip from 64bpp non scaled fb to 16bpp upscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_X_TILED, DRM_FORMAT_XRGB16161616F,
		I915_FORMAT_MOD_X_TILED, DRM_FORMAT_RGB565,
		0.5,
		1.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-64bpp-4tile-to-16bpp-4tile-upscaling",
		"Flip from 64bpp non scaled fb to 16bpp upscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_4_TILED, DRM_FORMAT_XRGB16161616F,
		I915_FORMAT_MOD_4_TILED, DRM_FORMAT_RGB565,
		0.5,
		1.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-64bpp-linear-to-16bpp-linear-upscaling",
		"Flip from 64bpp non scaled fb to 16bpp upscaled fb to stress CD clock programming",
		DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_XRGB16161616F,
		DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_RGB565,
		0.5,
		1.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-32bpp-ytileccs-to-64bpp-ytile-upscaling",
		"Flip from 32bpp non scaled fb to 64bpp upscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_Y_TILED_CCS, DRM_FORMAT_XRGB8888,
		I915_FORMAT_MOD_Y_TILED, DRM_FORMAT_XRGB16161616F,
		0.5,
		1.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-32bpp-yftileccs-to-64bpp-yftile-upscaling",
		"Flip from 32bpp non scaled fb to 64bpp upscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_Yf_TILED_CCS, DRM_FORMAT_XRGB8888,
		I915_FORMAT_MOD_Yf_TILED, DRM_FORMAT_XRGB16161616F,
		0.5,
		1.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-32bpp-ytile-to-32bpp-ytilegen12rcccs-upscaling",
		"Flip from 32bpp non scaled fb to 32bpp upscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_Y_TILED, DRM_FORMAT_XRGB8888,
		I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS, DRM_FORMAT_XRGB8888,
		0.5,
		1.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-32bpp-4tile-to-32bpp-4tiledg2rcccs-upscaling",
		"Flip from 32bpp non scaled fb to 32bpp upscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_4_TILED, DRM_FORMAT_XRGB8888,
		I915_FORMAT_MOD_4_TILED_DG2_RC_CCS, DRM_FORMAT_XRGB8888,
		0.5,
		1.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-32bpp-ytile-to-32bpp-ytileccs-upscaling",
		"Flip from 32bpp non scaled fb to 32bpp upscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_Y_TILED, DRM_FORMAT_XRGB8888,
		I915_FORMAT_MOD_Y_TILED_CCS, DRM_FORMAT_XRGB8888,
		0.5,
		1.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-32bpp-yftile-to-32bpp-yftileccs-upscaling",
		"Flip from 32bpp non scaled fb to 32bpp upscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_Yf_TILED, DRM_FORMAT_XRGB8888,
		I915_FORMAT_MOD_Yf_TILED_CCS, DRM_FORMAT_XRGB8888,
		0.5,
		1.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-64bpp-ytile-to-32bpp-ytilegen12rcccs-upscaling",
		"Flip from 64bpp non scaled fb to 32bpp upscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_Y_TILED, DRM_FORMAT_XRGB16161616F,
		I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS, DRM_FORMAT_XRGB8888,
		0.5,
		1.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-64bpp-4tile-to-32bpp-4tiledg2rcccs-upscaling",
		"Flip from 64bpp non scaled fb to 32bpp upscaled fb to stress CD clock programming",
		I915_FORMAT_MOD_4_TILED, DRM_FORMAT_XRGB16161616F,
		I915_FORMAT_MOD_4_TILED_DG2_RC_CCS, DRM_FORMAT_XRGB8888,
		0.5,
		1.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0,
	},
	{
		"flip-32bpp-linear-to-32bpp-linear-reflect-x",
		"Flip from 32bpp non scaled fb to 32bpp non scaled x mirrored fb",
		DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_XRGB8888,
		DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_XRGB8888,
		1.0,
		1.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0 | IGT_REFLECT_X,
	},
	{
		"flip-32bpp-yuv-linear-to-32bpp-yuv-linear-reflect-x",
		"Flip from 32bpp xyuv non scaled fb to 32bpp xyuv non scaled x mirrored fb",
		DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_XYUV8888,
		DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_XYUV8888,
		1.0,
		1.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0 | IGT_REFLECT_X,
	},
	{
		"flip-NV12-linear-to-NV12-linear-reflect-x",
		"Flip from NV12 non scaled fb to NV12 non scaled x mirrored fb",
		DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_NV12,
		DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_NV12,
		1.0,
		1.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0 | IGT_REFLECT_X,
	},
	{
		"flip-P016-linear-to-P016-linear-reflect-x",
		"Flip from P016 non scaled fb to P016 non scaled x mirrored fb",
		DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_P016,
		DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_P016,
		1.0,
		1.0,
		IGT_ROTATION_0,
		IGT_ROTATION_0 | IGT_REFLECT_X,
	},
};

static void setup_fb(data_t *data, struct igt_fb *newfb, uint32_t width,
		     uint32_t height, uint64_t format, uint64_t modifier)
{
	igt_create_color_fb(data->drm_fd, width, height,
			    format, modifier, 0, 1, 0, newfb);
}

static void free_fbs(data_t *data)
{
	igt_remove_fb(data->drm_fd, &data->small_fb);
	igt_remove_fb(data->drm_fd, &data->big_fb);
}

static void set_lut(data_t *data, igt_crtc_t *crtc)
{
	struct drm_color_lut *lut;
	drmModeCrtc *drm_crtc;
	int i, lut_size;

	drm_crtc = drmModeGetCrtc(data->drm_fd, crtc->crtc_id);
	lut_size = drm_crtc->gamma_size;
	drmModeFreeCrtc(drm_crtc);

	lut = malloc(sizeof(lut[0]) * lut_size);

	/*
	 * The scaler may have lower internal precision than
	 * the rest of the pipe. Limit the output to 8bpc using
	 * the legacy LUT.
	 */
	for (i = 0; i < lut_size; i++) {
		uint16_t v  = (i * 0xffff / (lut_size - 1)) & 0xff00;

		lut[i].red = v;
		lut[i].green = v;
		lut[i].blue = v;
	}

	igt_crtc_replace_prop_blob(crtc, IGT_CRTC_GAMMA_LUT,
				       lut, sizeof(lut[0]) * lut_size);

	free(lut);
}

static void clear_lut(data_t *data, igt_crtc_t *crtc)
{
	igt_crtc_set_prop_value(crtc, IGT_CRTC_GAMMA_LUT, 0);
}

static void test_flip_to_scaled(data_t *data, uint32_t index,
				igt_crtc_t *crtc, igt_output_t *output,
				drmModeModeInfoPtr modetoset, int flags)
{
	igt_plane_t *primary;
	igt_crc_t small_crc, big_crc;
	struct drm_event_vblank ev;
	int ret;

	igt_display_reset(&data->display);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	igt_debug("running on output %s pipe %s\n", output->name,
		  igt_crtc_name(crtc));

	if (data->big_fb.fb_id == 0) {
		setup_fb(data, &data->small_fb,
				data->attemptmodewidth * flip_scenario_test[index].firstmultiplier,
				data->attemptmodeheight * flip_scenario_test[index].firstmultiplier,
				flip_scenario_test[index].firstformat,
				flip_scenario_test[index].firstmodifier);

		setup_fb(data, &data->big_fb,
				data->attemptmodewidth * flip_scenario_test[index].secondmultiplier,
				data->attemptmodeheight * flip_scenario_test[index].secondmultiplier,
				flip_scenario_test[index].secondformat,
				flip_scenario_test[index].secondmodifier);

		igt_debug("small fb %dx%d\n", data->small_fb.width,
				data->small_fb.height);
		igt_debug("big fb %dx%d\n", data->big_fb.width,
				data->big_fb.height);
	}

	if (modetoset)
		igt_output_override_mode(output, modetoset);

	igt_output_set_crtc(output, crtc);

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

	if (data->gen >= 11) {
		igt_assert_f(igt_plane_has_prop(primary, IGT_PLANE_SCALING_FILTER),
			     "Plane scaling filter prop not supported!\n");
		igt_plane_set_prop_enum(primary, IGT_PLANE_SCALING_FILTER,
					kmstest_scaling_filter_str(flags));
	} else {
		igt_debug("Plane scaling filter prop not supported on gen < 11, running with default\n");
	}

	igt_skip_on_f(!igt_plane_has_format_mod(primary, data->small_fb.drm_format, data->small_fb.modifier) ||
		      !igt_plane_has_format_mod(primary, data->big_fb.drm_format,
		      data->big_fb.modifier), "No requested format/modifier on pipe %s\n",
		      igt_crtc_name(crtc));

	set_lut(data, crtc);
	igt_display_commit_atomic(&data->display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

	if (data->pipe_crc) {
		igt_pipe_crc_stop(data->pipe_crc);
		igt_pipe_crc_free(data->pipe_crc);
	}
	data->pipe_crc = igt_crtc_crc_new(crtc,
					  IGT_PIPE_CRC_SOURCE_AUTO);

	igt_plane_set_position(primary, 0, 0);
	igt_plane_set_fb(primary, &data->small_fb);
	igt_plane_set_size(primary, data->attemptmodewidth,
			   data->attemptmodeheight);
	igt_plane_set_rotation(primary,
			       flip_scenario_test[index].firstrotation);
	ret = igt_display_try_commit_atomic(&data->display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

	igt_skip_on_f(ret == -ERANGE, "Platform scaling limits exceeded, skipping.\n");
	igt_skip_on_f((ret == -EINVAL) && (!modetoset || modetoset->vrefresh > 90),
		      "Valid/default mode too big, cdclk limits exceeded. Check next connector\n");
	igt_skip_on_f((ret == -EINVAL) &&
		      (flip_scenario_test[index].firstrotation != IGT_ROTATION_0),
		      "Unsupported rotation\n");
	igt_assert_eq(ret, 0);

	igt_pipe_crc_start(data->pipe_crc);
	igt_pipe_crc_get_current(data->drm_fd, data->pipe_crc, &small_crc);

	igt_plane_set_fb(primary, &data->big_fb);
	igt_plane_set_size(primary, data->attemptmodewidth,
			   data->attemptmodeheight);
	igt_plane_set_rotation(primary,
			       flip_scenario_test[index].secondrotation);
	ret = igt_display_try_commit_atomic(&data->display,
					    DRM_MODE_ATOMIC_ALLOW_MODESET  |
					    DRM_MODE_PAGE_FLIP_EVENT, NULL);

	igt_skip_on_f(ret == -ERANGE, "Platform scaling limits exceeded, skipping.\n");
	igt_skip_on_f((ret == -EINVAL) && (!modetoset || modetoset->vrefresh > 90),
		      "Valid/default mode too big, cdclk limits exceeded. Check next connector\n");
	igt_skip_on_f((ret == -EINVAL) &&
		      (flip_scenario_test[index].secondrotation != IGT_ROTATION_0),
		      "Unsupported rotation\n");
	igt_assert_eq(ret, 0);

	igt_assert(read(data->drm_fd, &ev, sizeof(ev)) == sizeof(ev));

	igt_pipe_crc_get_current(data->drm_fd, data->pipe_crc, &big_crc);
	igt_assert_crc_equal(&small_crc, &big_crc);

	igt_pipe_crc_stop(data->pipe_crc);
	igt_pipe_crc_free(data->pipe_crc);
	data->pipe_crc = NULL;

	clear_lut(data, crtc);

	modetoset = NULL;
	igt_output_set_crtc(output, NULL);
	igt_plane_set_fb(primary, NULL);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);
}

static drmModeModeInfoPtr find_mode(data_t *data, igt_output_t *output)
{
	drmModeModeInfoPtr modetoset = NULL;

	for (int i = 0; i < output->config.connector->count_modes; i++) {
		if (output->config.connector->modes[i].hdisplay == data->attemptmodewidth &&
		    output->config.connector->modes[i].vdisplay == data->attemptmodeheight) {
			if (modetoset &&
			    modetoset->vrefresh < output->config.connector->modes[i].vrefresh)
				continue;

			data->mode = output->config.connector->modes[i];
			modetoset = &data->mode;
		}
	}

	return modetoset;
}

static void run_tests(data_t *data, uint32_t index, igt_crtc_t *crtc,
		      igt_output_t * output, drmModeModeInfoPtr modetoset)
{
	test_flip_to_scaled(data, index, crtc,
			    output, modetoset, 0);

	/*
	 * test Nearest Neighbor filter. For scaler indexes see
	 * scaling_filter_names structure in igt_kms.c
	 * Platform scaling filter property is supported only gen >= 11.
	 */
	if (data->gen >= 11)
		test_flip_to_scaled(data, index,
				    crtc, output,
				    modetoset, 1);
}

int igt_main()
{
	igt_crtc_t *crtc;
	data_t data = {};
	igt_output_t *output;
	drmModeModeInfoPtr modetoset = NULL;

	igt_fixture() {
		data.drm_fd = drm_open_driver_master(DRIVER_INTEL | DRIVER_XE);
		data.gen = intel_display_ver(intel_get_drm_devid(data.drm_fd));
		igt_require(data.gen >= 9);
		igt_display_require(&data.display, data.drm_fd);
		igt_require(data.display.is_atomic);
		igt_require_pipe_crc(data.drm_fd);
		kmstest_set_vt_graphics_mode();

		if (data.gen < 11) {
			data.attemptmodewidth = 640;
			data.attemptmodeheight = 480;
		} else {
			data.attemptmodewidth = 1920;
			data.attemptmodeheight = 1080;
		}
	}

	igt_describe("Tests scaler using default and nearest neighbor plane scaling filters");
	for (int index = 0; index < ARRAY_SIZE(flip_scenario_test); index++) {
		igt_describe(flip_scenario_test[index].describe);
		igt_subtest_with_dynamic(flip_scenario_test[index].name) {
			igt_require(igt_display_has_format_mod(&data.display,
							       flip_scenario_test[index].firstformat,
							       flip_scenario_test[index].firstmodifier));
			igt_require(igt_display_has_format_mod(&data.display,
							       flip_scenario_test[index].secondformat,
							       flip_scenario_test[index].secondmodifier));

			if (flip_scenario_test[index].secondmodifier == DRM_FORMAT_MOD_LINEAR &&
			    flip_scenario_test[index].secondrotation & IGT_REFLECT_X)
				igt_require_f(data.gen >= 35,
					      "Linear fb with REFLECT_X unsupported\n");

			free_fbs(&data);
			for_each_crtc(&data.display, crtc) {
				bool found = false;
				for_each_valid_output_on_pipe(&data.display,
							      crtc->pipe,
							      output) {
					igt_display_reset(&data.display);

					modetoset = find_mode(&data, output);
					igt_output_set_crtc(output,
							    crtc);
					igt_output_override_mode(output, modetoset);

					if (modetoset && intel_pipe_output_combo_valid(&data.display)) {
						found = true;
						igt_dynamic_f("pipe-%s-valid-mode",
							      igt_crtc_name(crtc))
							run_tests(&data,
								  index,
								  crtc,
								  output,
								  modetoset);
						break;
					}
				}
				if (!found) {
					for_each_valid_output_on_pipe(&data.display,
								      crtc->pipe,
								      output) {
						igt_display_reset(&data.display);

						igt_output_set_crtc(output,
								    crtc);
						if (!intel_pipe_output_combo_valid(&data.display))
							continue;

						modetoset = NULL;
						igt_dynamic_f("pipe-%s-default-mode",
							      igt_crtc_name(crtc))
							run_tests(&data,
								  index,
								  crtc,
								  output,
								  modetoset);
					}
				}
				break;
			}
		}
	}

	igt_fixture() {
		free_fbs(&data);
		if (data.pipe_crc) {
			igt_pipe_crc_stop(data.pipe_crc);
			igt_pipe_crc_free(data.pipe_crc);
			data.pipe_crc = NULL;
		}
		kmstest_set_vt_text_mode();
		igt_display_fini(&data.display);
		drm_close_driver(data.drm_fd);
	}
}
