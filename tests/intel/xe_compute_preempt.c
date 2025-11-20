// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

/**
 * TEST: Check compute-related preemption functionality
 * Category: Core
 * Mega feature: WMTP
 * Sub-category: wmtp tests
 * Functionality: OpenCL kernel
 * Test category: functionality test
 */

#include <inttypes.h>
#include <string.h>

#include "igt.h"
#include "intel_compute.h"
#include "xe/xe_query.h"

/**
 * SUBTEST: compute-preempt
 * GPU requirement: LNL, PTL
 * Description:
 *      Exercise compute walker mid thread preemption scenario
 *
 * SUBTEST: compute-preempt-many
 * GPU requirement: LNL, PTL
 * Description:
 *      Exercise multiple walker mid thread preemption scenario
 *
 * SUBTEST: compute-preempt-many-all-ram
 * GPU requirement: LNL, PTL
 * Description:
 *      Exercise multiple walker mid thread preemption scenario consuming
 *      whole ram only when there's swap on the machine
 *
 * SUBTEST: compute-preempt-many-vram
 * GPU requirement: LNL, PTL
 * Description:
 *      Exercise multiple walker mid thread preemption scenario on half of vram
 *
 * SUBTEST: compute-preempt-many-vram-evict
 * GPU requirement: LNL, PTL
 * Description:
 *      Exercise multiple walker mid thread preemption scenario on 120% of vram size
 *
 * SUBTEST: compute-threadgroup-preempt
 * GPU requirement: LNL, PTL
 * Description:
 *      Exercise compute walker threadgroup preemption scenario
 */
static void
test_compute_preempt(int fd, struct drm_xe_engine_class_instance *hwe, bool threadgroup_preemption,
		     enum execenv_alloc_prefs alloc_prefs)
{
	igt_require_f(run_intel_compute_kernel_preempt(fd, hwe, threadgroup_preemption,
						       alloc_prefs),
		      "GPU not supported\n");
}

#define CONTEXT_MB 100

int igt_main()
{
	int xe;
	struct drm_xe_engine_class_instance *hwe;
	uint64_t ram_mb, swap_mb, vram_mb;

	igt_fixture() {
		xe = drm_open_driver(DRIVER_XE);
		ram_mb = igt_get_avail_ram_mb();
		swap_mb = igt_get_total_swap_mb();
		vram_mb = xe_visible_vram_size(xe, 0) >> 20;
	}

	igt_subtest_with_dynamic("compute-preempt") {
		igt_require(xe_kernel_preempt_check(xe, PREEMPT_WMTP));
		xe_for_each_engine(xe, hwe) {
			if (hwe->engine_class != DRM_XE_ENGINE_CLASS_COMPUTE)
				continue;

			igt_dynamic_f("engine-%s", xe_engine_class_string(hwe->engine_class))
				test_compute_preempt(xe, hwe, false, EXECENV_PREF_SYSTEM);
		}
	}

	igt_subtest_with_dynamic("compute-preempt-many") {
		igt_require(xe_kernel_preempt_check(xe, PREEMPT_WMTP));
		xe_for_each_engine(xe, hwe) {
			if (hwe->engine_class != DRM_XE_ENGINE_CLASS_COMPUTE)
				continue;

			igt_dynamic_f("engine-%s", xe_engine_class_string(hwe->engine_class)) {
				int child_count;

				/*
				 * Get half of ram, then divide by
				 * CONTEXT_MB * 2 (long and short) job
				 */
				child_count = ram_mb / 2 / CONTEXT_MB / 2;

				igt_debug("RAM: %" PRIu64 ", child count: %d\n",
					  ram_mb, child_count);

				igt_fork(child, child_count)
					test_compute_preempt(xe, hwe, false, EXECENV_PREF_SYSTEM);
				igt_waitchildren();
			}
		}
	}

	igt_subtest_with_dynamic("compute-preempt-many-all-ram") {
		igt_require(swap_mb > CONTEXT_MB * 10);

		igt_require(xe_kernel_preempt_check(xe, PREEMPT_WMTP));
		xe_for_each_engine(xe, hwe) {
			if (hwe->engine_class != DRM_XE_ENGINE_CLASS_COMPUTE)
				continue;

			igt_dynamic_f("engine-%s", xe_engine_class_string(hwe->engine_class)) {
				int child_count;

				/*
				 * Get whole ram, then divide by
				 * CONTEXT_MB * 2 (long and short) job
				 */
				child_count = ram_mb / CONTEXT_MB / 2;

				igt_debug("RAM: %" PRIu64 ", child count: %d\n",
					  ram_mb, child_count);

				igt_fork(child, child_count)
					test_compute_preempt(xe, hwe, false, EXECENV_PREF_SYSTEM);
				igt_waitchildren();
			}
		}
	}

	igt_subtest_with_dynamic("compute-preempt-many-vram") {
		igt_require(xe_has_vram(xe));

		igt_require(xe_kernel_preempt_check(xe, PREEMPT_WMTP));
		xe_for_each_engine(xe, hwe) {
			if (hwe->engine_class != DRM_XE_ENGINE_CLASS_COMPUTE)
				continue;

			igt_dynamic_f("engine-%s", xe_engine_class_string(hwe->engine_class)) {
				int child_count;

				/*
				 * Get half of vram, then divide by
				 * CONTEXT_MB * 2 (long and short) job
				 */
				child_count = vram_mb / 2 / CONTEXT_MB / 2;

				igt_debug("VRAM: %" PRIu64 ", child count: %d\n",
					  vram_mb, child_count);

				igt_fork(child, child_count)
					test_compute_preempt(xe, hwe, false, EXECENV_PREF_VRAM);
				igt_waitchildren();
			}
		}
	}

	igt_subtest_with_dynamic("compute-preempt-many-vram-evict") {
		igt_require(xe_has_vram(xe));

		igt_require(xe_kernel_preempt_check(xe, PREEMPT_WMTP));
		xe_for_each_engine(xe, hwe) {
			if (hwe->engine_class != DRM_XE_ENGINE_CLASS_COMPUTE)
				continue;

			igt_dynamic_f("engine-%s", xe_engine_class_string(hwe->engine_class)) {
				int child_count;

				/*
				 * Get all vram + 20%, then divide by
				 * CONTEXT_MB * 2 (long and short) job
				 */
				child_count = vram_mb * 1.2 / 2 / CONTEXT_MB;

				igt_debug("VRAM: %" PRIu64 ", child count: %d\n",
					  vram_mb, child_count);

				igt_fork(child, child_count)
					test_compute_preempt(xe, hwe, false, EXECENV_PREF_VRAM);
				igt_waitchildren();
			}
		}
	}

	igt_subtest_with_dynamic("compute-threadgroup-preempt") {
		igt_require(xe_kernel_preempt_check(xe, PREEMPT_TGP));
		xe_for_each_engine(xe, hwe) {
			if (hwe->engine_class != DRM_XE_ENGINE_CLASS_COMPUTE)
				continue;

			igt_dynamic_f("engine-%s", xe_engine_class_string(hwe->engine_class))
			test_compute_preempt(xe, hwe, true, EXECENV_PREF_SYSTEM);
		}
	}

	igt_fixture()
		drm_close_driver(xe);

}
