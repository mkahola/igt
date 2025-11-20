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
 */

#include "igt.h"
#include "igt_kmod.h"
/**
 * TEST: drm mm
 * Description: Basic sanity check of DRM's range manager (struct drm_mm)
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Memory management tests
 * Functionality: drm_mm
 * Feature: mapping
 * Test category: GEM_Legacy
 *
 * SUBTEST: drm_mm
 *
 * SUBTEST: drm_mm@align
 *
 * SUBTEST: drm_mm@align32
 *
 * SUBTEST: drm_mm@align64
 *
 * SUBTEST: drm_mm@bottomup
 *
 * SUBTEST: drm_mm@color
 *
 * SUBTEST: drm_mm@color_evict
 *
 * SUBTEST: drm_mm@color_evict_range
 *
 * SUBTEST: drm_mm@debug
 *
 * SUBTEST: drm_mm@evict
 *
 * SUBTEST: drm_mm@evict_range
 *
 * SUBTEST: drm_mm@frag
 *
 * SUBTEST: drm_mm@highest
 *
 * SUBTEST: drm_mm@init
 *
 * SUBTEST: drm_mm@insert
 *
 * SUBTEST: drm_mm@insert_range
 *
 * SUBTEST: drm_mm@lowest
 *
 * SUBTEST: drm_mm@replace
 *
 * SUBTEST: drm_mm@reserve
 *
 * SUBTEST: drm_mm@sanitycheck
 *
 * SUBTEST: drm_mm@topdown
 */

IGT_TEST_DESCRIPTION("Basic sanity check of DRM's range manager (struct drm_mm)");

igt_main()
{
	igt_kunit("drm_mm_test", NULL, NULL);
}
