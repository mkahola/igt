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
 * TEST: i915 selftest
 * Description: Basic unit tests for i915.ko
 *
 * SUBTEST: live
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Selftest subcategory
 * Functionality: live selftest
 * Feature: gem_core
 *
 * SUBTEST: live@active
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Synchronization
 * Functionality: semaphore
 * Test category: i915
 *
 * SUBTEST: live@blt
 * Description: Blitter validation
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Blitter tests
 * Functionality: command streamer
 * Test category: i915 / HW
 *
 * SUBTEST: live@client
 * Description: Internal API over blitter
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: uapi
 * Functionality: blitter api
 * Test category: i915
 *
 * SUBTEST: live@coherency
 * Description: Cache management
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Memory management tests
 * Functionality: cache
 * Test category: i915 / HW
 *
 * SUBTEST: live@debugger
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Debugging
 * Functionality: device management
 * Test category: debugger
 *
 * SUBTEST: live@display
 * Category: Core
 * Mega feature: General Display Features
 * Sub-category: Display tests
 * Functionality: display sanity
 * Test category: i915
 *
 * SUBTEST: live@dmabuf
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Memory management tests
 * Functionality: dmabuf test
 * Test category: i915
 *
 * SUBTEST: live@evict
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Memory management tests
 * Functionality: GTT eviction
 * Test category: i915
 *
 * SUBTEST: live@execlists
 * Description: command submission backend
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: CMD submission
 * Functionality: execlists
 * Test category: i915
 *
 * SUBTEST: live@gem
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: CMD submission
 * Functionality: execbuf
 * Test category: i915
 *
 * SUBTEST: live@gem_contexts
 * Description: User isolation and execution at the context level
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: CMD submission
 * Functionality: context
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@gem_execbuf
 * Description: command submission support
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: CMD submission
 * Functionality: execbuf
 * Test category: i915
 *
 * SUBTEST: live@gt_ccs_mode
 * Description: Multi-ccs internal validation
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: CMD submission
 * Functionality: multii-ccs
 * Test category: i915
 *
 * SUBTEST: live@gt_contexts
 * Description: HW isolation and HW context validation
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: CMD submission
 * Functionality: context
 * Test category: HW
 *
 * SUBTEST: live@gt_engines
 * Description: command submission topology validation
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: CMD submission
 * Functionality: command submission topology
 * Test category: i915
 *
 * SUBTEST: live@gt_gtt
 * Description: Validation of virtual address management and execution
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Memory management tests
 * Functionality: gtt
 * Test category: HW
 *
 * SUBTEST: live@gt_heartbeat
 * Description: Stall detection interface validation
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Reset tests
 * Functionality: heartbeat
 * Test category: i915
 *
 * SUBTEST: live@gt_lrc
 * Description: HW isolation and HW context validation
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: CMD submission
 * Functionality: context
 * Test category: HW
 *
 * SUBTEST: live@gt_mocs
 * Description: Verification of mocs registers
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Mocs
 * Functionality: mocs registers
 * Test category: i915 / HW
 *
 * SUBTEST: live@gt_pm
 * Description: Basic i915 driver module selftests
 * Category: Core
 * Mega feature: Power management
 * Sub-category: Power management tests
 * Functionality: rps, rc6
 *
 * SUBTEST: live@gt_timelines
 * Description: semaphore tracking
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Synchronization
 * Functionality: semaphore
 * Test category: i915
 *
 * SUBTEST: live@gt_tlb
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Memory management tests
 * Functionality: tlb
 * Test category: Memory Management
 *
 * SUBTEST: live@gtt
 * Description: Virtual address management interface validation
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Memory management tests
 * Functionality: virtual address
 * Test category: i915
 *
 * SUBTEST: live@gtt_l4wa
 * Description: Check the L4WA is enabled when it was required
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Workarounds
 * Functionality: L4WA
 * Test category: i915
 *
 * SUBTEST: live@guc
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Firmware tests
 * Functionality: GUC
 * Feature: GuC
 * Test category: GuC
 *
 * SUBTEST: live@guc_doorbells
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Firmware tests
 * Functionality: GUC
 * Feature: GuC
 * Test category: GuC
 *
 * SUBTEST: live@guc_hang
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Firmware tests
 * Functionality: GUC
 * Feature: GuC
 * Test category: GuC
 *
 * SUBTEST: live@guc_multi_lrc
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Firmware tests
 * Functionality: GUC
 * Feature: GuC
 * Test category: GuC
 *
 * SUBTEST: live@hangcheck
 * Description: reset handling after stall detection
 * Category: Core
 * Mega feature: General Core features
 * Functionality: hangcheck
 * Test category: i915
 * Sub-category: Reset tests
 *
 * SUBTEST: live@hugepages
 * Description: Large page support validation
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Memory management tests
 * Functionality: large page
 * Test category: i915
 *
 * SUBTEST: live@late_gt_pm
 * Description: Basic i915 driver module selftests
 * Category: Core
 * Mega feature: Power management
 * Sub-category: Power management tests
 * Feature: rc6
 * Functionality: rc6
 *
 * SUBTEST: live@lmem
 * Description: Basic i915 driver module selftests
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Memory management tests
 * Feature: local_memory
 * Functionality: local memory
 *
 * SUBTEST: live@memory_region
 * Description: memory topology validation and migration checks
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Memory management tests
 * Functionality: memory topology
 * Test category: i915 / HW
 *
 * SUBTEST: live@memory_region_cross_tile
 * Category: Core
 * Mega feature: General Core features
 * Description: Multi-tile memory topology validation
 * Category: Core
 * Sub-category: MultiTile
 * Functionality: memory topology
 *
 * SUBTEST: live@mman
 * Description: memory management validation
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Memory management tests
 * Functionality: mapping
 * Test category: i915
 *
 * SUBTEST: live@obj_lock
 * Description: Validation of per-object locking patterns
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: CMD submission
 * Functionality: per-object locking
 * Test category: i915
 *
 * SUBTEST: live@objects
 * Description: User object allocation and isolation checks
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: CMD submission
 * Functionality: objects
 * Test category: i915
 *
 * SUBTEST: live@perf
 * Description: Basic i915 module perf unit selftests
 * Category: Core
 * Mega feature: Performance interface
 * Sub-category: Performance tests
 * Functionality: perf
 * Feature: i915 perf selftests
 *
 * SUBTEST: live@remote_tiles
 * Description: Tile meta data validation
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: MultiTile
 * Functionality: meta data
 *
 * SUBTEST: live@requests
 * Description: Validation of internal i915 command submission interface
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: CMD submission
 * Functionality: command submission interface
 * Test category: i915
 *
 * SUBTEST: live@reset
 * Description: engine/GT resets
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Reset tests
 * Functionality: engine/GT reset
 * Test category: HW
 *
 * SUBTEST: live@sanitycheck
 * Description: Checks the selftest infrastructure itself
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: driver
 * Functionality: sanitycheck
 * Test category: i915
 *
 * SUBTEST: live@scheduler
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: CMD Submission
 * Functionality: scheduler
 *
 * SUBTEST: live@semaphores
 * Description: GuC semaphore management
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Synchronization
 * Functionality: semaphore
 * Test category: HW
 *
 * SUBTEST: live@slpc
 * Description: Basic i915 driver module selftests
 * Category: Core
 * Mega feature: Power management
 * Sub-category: Power management tests
 * Functionality: slpc
 * Feature: slpc / pm_rps
 *
 * SUBTEST: live@uncore
 * Description: Basic i915 driver module selftests
 * Category: Core
 * Mega feature: Power management
 * Sub-category: Power management tests
 * Functionality: forcewake
 * Feature: forcewake
 *
 * SUBTEST: live@vma
 * Description: Per-object virtual address management
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Memory management tests
 * Functionality: virtual address
 * Test category: i915
 *
 * SUBTEST: live@win_blt_copy
 * Description: Validation of migration interface
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Blitter tests
 * Functionality: migration interface
 * Test category: i915 / HW
 *
 * SUBTEST: live@workarounds
 * Description: Check workarounds persist or are reapplied after resets and other power management events
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Workarounds
 * Functionality: driver workarounds
 * Test category: HW
 *
 * SUBTEST: mock
 * Category: Core
 * Mega feature: General Core features
 * Feature: gem_core
 * Sub-category: Selftest subcategory
 * Functionality: mock selftest
 * Feature: gem_core
 *
 * SUBTEST: mock@buddy
 * Description: Buddy allocation
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Memory management tests
 * Functionality: buddy allocation
 * Test category: DRM
 *
 * SUBTEST: mock@contexts
 * Description: GEM context internal API checks
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: uapi
 * Functionality: context
 * Test category: i915
 *
 * SUBTEST: mock@dmabuf
 * Description: dma-buf (buffer management) API checks
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: uapi
 * Functionality: dmabuf test
 * Test category: DRM
 *
 * SUBTEST: mock@engine
 * Description: Engine topology API checks
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: uapi
 * Functionality: engine topology
 * Test category: i915
 *
 * SUBTEST: mock@evict
 * Description: GTT eviction API checks
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: uapi
 * Functionality: gtt eviction
 * Test category: i915
 *
 * SUBTEST: mock@fence
 * Description: semaphore API checks
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: uapi
 * Functionality: semaphore api
 * Test category: i915
 *
 * SUBTEST: mock@gtt
 * Description: Virtual address management API checks
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: uapi
 * Functionality: gtt
 * Test category: i915
 *
 * SUBTEST: mock@hugepages
 * Description: Hugepage API checks
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: uapi
 * Functionality: huge page
 * Test category: i915
 *
 * SUBTEST: mock@memory_region
 * Description: Memory region API checks
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: uapi
 * Functionality: memory region
 * Test category: i915
 *
 * SUBTEST: mock@objects
 * Description: Buffer object API checks
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: uapi
 * Functionality: buffer object
 * Test category: i915
 *
 * SUBTEST: mock@phys
 * Description: legacy physical object API checks
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: uapi
 * Functionality: physical object
 * Test category: i915
 *
 * SUBTEST: mock@requests
 * Description: Internal command submission API checks
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: uapi
 * Functionality: requests
 * Test category: i915
 *
 * SUBTEST: mock@ring
 * Description: Ringbuffer management API checks
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: uapi
 * Functionality: ringbuffer
 * Test category: i915
 *
 * SUBTEST: mock@sanitycheck
 * Description: Selftest for the selftest
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: driver
 * Functionality: sanitycheck
 * Test category: i915
 *
 * SUBTEST: mock@scatterlist
 * Description: Scatterlist API checks
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: uapi
 * Functionality: scatterlist
 * Test category: i915
 *
 * SUBTEST: mock@shmem
 * Description: SHM utils API checks
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: uapi
 * Functionality: shm
 * Test category: i915
 *
 * SUBTEST: mock@syncmap
 * Description: API checks for the contracted radixtree
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: uapi
 * Functionality: contracted radixtree
 * Test category: i915
 *
 * SUBTEST: mock@timelines
 * Description: API checks for semaphore tracking
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: uapi
 * Functionality: semaphore api
 * Test category: i915
 *
 * SUBTEST: mock@tlb
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Memory management tests
 * Functionality: tlb
 * Test category: Memory Management
 *
 * SUBTEST: mock@uncore
 * Description: Basic i915 driver module selftests
 * Category: Core
 * Mega feature: Power management
 * Sub-category: Power management tests
 * Functionality: forcewake
 * Feature: forcewake
 *
 * SUBTEST: mock@vma
 * Description: API checks for virtual address management
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: uapi
 * Functionality: virtual address
 * Test category: i915
 *
 * SUBTEST: perf
 * Category: Core
 * Mega feature: Performance interface
 * Sub-category: Performance tests
 * Functionality: oa
 * Feature: oa
 *
 * SUBTEST: perf@blt
 * Description: Basic i915 module perf unit selftests
 * Category: Core
 * Mega feature: Performance interface
 * Sub-category: Performance tests
 * Functionality: perf
 * Feature: i915 perf selftests
 *
 * SUBTEST: perf@engine_cs
 * Description: Basic i915 module perf unit selftests
 * Category: Core
 * Mega feature: Performance interface
 * Sub-category: Performance tests
 * Functionality: perf
 * Feature: i915 perf selftests
 *
 * SUBTEST: perf@region
 * Description: Basic i915 module perf unit selftests
 * Category: Core
 * Mega feature: Performance interface
 * Sub-category: Performance tests
 * Functionality: perf
 * Feature: i915 perf selftests
 *
 * SUBTEST: perf@request
 * Description: Basic i915 module perf unit selftests
 * Category: Core
 * Mega feature: Performance interface
 * Sub-category: Performance tests
 * Functionality: perf
 *
 * SUBTEST: perf@scheduler
 * Description: Basic i915 module perf unit selftests
 * Category: Core
 * Mega feature: Performance interface
 * Sub-category: Performance tests
 * Functionality: perf
 */

IGT_TEST_DESCRIPTION("Basic unit tests for i915.ko");

int igt_main()
{
	const char *env = getenv("SELFTESTS") ?: "";
	char opts[1024];

	igt_assert(snprintf(opts, sizeof(opts),
			    "mock_selftests=-1 disable_display=1 st_filter=%s",
			    env) < sizeof(opts));
	igt_kselftests("i915", opts, NULL, "mock", NULL);

	igt_assert(snprintf(opts, sizeof(opts),
			    "live_selftests=-1 disable_display=1 st_filter=%s",
			    env) < sizeof(opts));
	igt_kselftests("i915", opts, "live_selftests", "live", NULL);

	igt_assert(snprintf(opts, sizeof(opts),
			    "perf_selftests=-1 disable_display=1 st_filter=%s",
			    env) < sizeof(opts));
	igt_kselftests("i915", opts, "perf_selftests", "perf", NULL);
}
