// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

/**
 * TEST: Check compute-related functionality
 * Category: Core
 * Mega feature: Compute
 * Sub-category: Compute tests
 * Test category: functionality test
 */

#include <string.h>

#include "igt.h"
#include "intel_compute.h"

/**
 * SUBTEST: compute-square
 * GPU requirement: TGL, DG2, ATS-M
 * Description:
 *	Run an openCL Kernel that returns output[i] = input[i] * input[i],
 *	for an input dataset..
 * Functionality: OpenCL kernel
 */
static void
test_compute_square(int fd)
{
	igt_require_f(run_intel_compute_kernel(fd, NULL, EXECENV_PREF_SYSTEM),
		      "GPU not supported\n");
}

igt_main
{
	int i915;

	igt_fixture()
		i915 = drm_open_driver(DRIVER_INTEL);

	igt_subtest("compute-square")
		test_compute_square(i915);

	igt_fixture()
		drm_close_driver(i915);
}
