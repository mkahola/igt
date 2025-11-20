// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include <unistd.h>

#include "drmtest.h"
#include "igt.h"

#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"

#define NUM_VMS 10
#define NUM_BOS 1
#define SECONDS_TO_WAIT 10

/**
 * TEST: Expose compute VM's unnecessary rebinds
 * Category: Core
 * Mega feature: Compute
 * Sub-category: Compute tests
 * Functionality: VM rebind
 * Test category: functionality test
 *
 * SUBTEST: basic
 * Description:
 *	This test creates compute vms, binds a couple of bos and an exec_queue each,
 *	thus redying it for execution.
 *
 */
 /*
  * More dailed test description:
  *	This test creates compute vms, binds a couple of bos and an exec_queue each,
  *	thus redying it for execution. However, VRAM memory is over-
  *	committed and while there is still nothing to execute, an eviction
  *	will trigger the VM's rebind worker to rebind the evicted bo, which
  *	will in turn trigger another eviction and so on.
  *
  *	Since we don't have eviction stats yet we need to watch "top" for
  *	the rebind kworkers using a lot of CPU while the test idles.
  *
  *	The correct driver behaviour should be not to rebind anything unless
  *	there is worked queued on one of the VM's compute exec_queues.
 */

static void test_ping_pong(int fd, struct drm_xe_engine *engine)
{
	size_t vram_size = xe_vram_size(fd, 0);
	size_t align = xe_get_default_alignment(fd);
	size_t bo_size = vram_size / NUM_VMS / NUM_BOS;
	uint32_t vm[NUM_VMS];
	uint32_t bo[NUM_VMS][NUM_BOS];
	uint32_t exec_queues[NUM_VMS];
	unsigned int i, j;

	igt_skip_on(!bo_size);

	/* Align and make sure we overcommit vram with at least 10% */
	bo_size = ALIGN(bo_size + bo_size / 10, align);

	/*
	 * This should not start ping-ponging memory between system and
	 * VRAM. For now look at top to determine. TODO: Look at eviction
	 * stats.
	 */
	for (i = 0; i < NUM_VMS; ++i) {
		vm[i] = xe_vm_create(fd, DRM_XE_VM_CREATE_FLAG_LR_MODE, 0);
		for (j = 0; j < NUM_BOS; ++j) {
			igt_debug("Creating bo size %lu for vm %u\n",
				  (unsigned long) bo_size,
				  (unsigned int) vm[i]);

			bo[i][j] = xe_bo_create(fd, vm[i], bo_size,
						vram_memory(fd, 0), 0);
			xe_vm_bind_async(fd, vm[i], 0, bo[i][j], 0,
					 0x40000 + j*bo_size, bo_size, NULL, 0);
		}
		exec_queues[i] = xe_exec_queue_create(fd, vm[i],
						      &engine->instance, 0);
	}

	igt_info("Now sleeping for %ds.\n", SECONDS_TO_WAIT);
	igt_info("Watch \"top\" for high-cpu kworkers!\n");
	sleep(SECONDS_TO_WAIT);

	for (i = 0; i < NUM_VMS; ++i) {
		xe_exec_queue_destroy(fd, exec_queues[i]);
		for (j = 0; j < NUM_BOS; ++j)
			gem_close(fd, bo[i][j]);
		xe_vm_destroy(fd, vm[i]);
	}
}

static int fd;

IGT_TEST_DESCRIPTION("Expose compute VM's unnecessary rebinds");
igt_main()
{
	igt_fixture()
		fd = drm_open_driver(DRIVER_XE);

	igt_describe("Check for unnnecessary rebinds");
	igt_subtest("basic")
		test_ping_pong(fd, xe_engine(fd, 0));

	igt_fixture()
		drm_close_driver(fd);
}
