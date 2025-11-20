// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

/**
 * TEST: sysfs preempt timeout
 * Category: Core
 * Mega feature: SysMan
 * Sub-category: SysMan tests
 * Functionality: sysfs preempt timeout
 * Feature: SMI, context
 * Test category: SysMan
 *
 * SUBTEST: preempt_timeout_us-timeout
 * Description: Test to measure the delay from requesting the preemption to its
 *      completion. Send down some non-preemptable workloads and then
 *      request a switch to a higher priority context. The HW will not
 *      be able to respond, so the kernel will be forced to reset the hog.
 * Test category: functionality test
 *
 */

#include <fcntl.h>

#include "igt.h"
#include "igt_syncobj.h"
#include "igt_sysfs.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_spin.h"

#define ATTR "preempt_timeout_us"

static void set_preempt_timeout(int engine, unsigned int value)
{
	unsigned int delay;

	igt_assert_lte(0, igt_sysfs_printf(engine, ATTR, "%u", value));
	igt_sysfs_scanf(engine, ATTR, "%u", &delay);
	igt_assert_eq(delay, value);
}

static uint64_t __test_timeout(int fd, int engine, unsigned int timeout, uint16_t gt, int class)
{
	struct drm_xe_sync sync = {
		.handle = syncobj_create(fd, 0),
		.type = DRM_XE_SYNC_TYPE_SYNCOBJ,
		.flags = DRM_XE_SYNC_FLAG_SIGNAL,
	};

	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 1,
		.syncs = to_user_pointer(&sync),
	};
/* high priority property */
	struct drm_xe_ext_set_property ext = {
		.base.next_extension = 0,
		.base.name = DRM_XE_EXEC_QUEUE_EXTENSION_SET_PROPERTY,
		.property = DRM_XE_EXEC_QUEUE_SET_PROPERTY_PRIORITY,
		.value = 2, /* High priority */
	};
	struct drm_xe_engine_class_instance *hwe = NULL, *_hwe;
	uint64_t ahnd[2];
	uint32_t exec_queues[2];
	uint32_t vm[2];
	uint32_t bo[2];
	size_t bo_size;
	struct xe_spin *spin[2];
	struct timespec ts = {};
	double elapsed;
	uint64_t addr1 = 0x1a0000, addr2 = 0x100000;

	xe_for_each_engine(fd, _hwe)
		if (_hwe->engine_class == class && _hwe->gt_id == gt)
			hwe = _hwe;

	if (!hwe)
		return -1;

	/* set preempt timeout*/
	set_preempt_timeout(engine, timeout);
	vm[0] = xe_vm_create(fd, 0, 0);
	vm[1] = xe_vm_create(fd, 0, 0);
	exec_queues[0] = xe_exec_queue_create(fd, vm[0], hwe, 0);
	exec_queues[1] = xe_exec_queue_create(fd, vm[1], hwe, to_user_pointer(&ext));
	ahnd[0] = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_RELOC);
	bo_size = xe_bb_size(fd, sizeof(*spin));
	bo[0] = xe_bo_create(fd, vm[0], bo_size, vram_if_possible(fd, 0),
			     DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	spin[0] = xe_bo_map(fd, bo[0], bo_size);
	xe_vm_bind_async(fd, vm[0], 0, bo[0], 0, addr1, bo_size, &sync, 1);
	xe_spin_init_opts(spin[0], .addr = addr1,
				.preempt = false);
	exec.address = addr1;
	exec.exec_queue_id = exec_queues[0];
	xe_exec(fd, &exec);
	xe_spin_wait_started(spin[0]);

	igt_nsec_elapsed(&ts);
	ahnd[1] = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_RELOC);
	bo[1] = xe_bo_create(fd, vm[1], bo_size, vram_if_possible(fd, 0),
			     DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	spin[1] = xe_bo_map(fd, bo[1], bo_size);
	xe_vm_bind_sync(fd, vm[1], bo[1], 0, addr2, bo_size);
	xe_spin_init_opts(spin[1], .addr = addr2);
	exec.address = addr2;
	exec.exec_queue_id = exec_queues[1];
	xe_exec(fd, &exec);
	xe_spin_wait_started(spin[1]);
	elapsed = igt_nsec_elapsed(&ts);
	xe_spin_end(spin[1]);

	xe_vm_unbind_async(fd, vm[0], 0, 0, addr1, bo_size, &sync, 1);
	igt_assert(syncobj_wait(fd, &sync.handle, 1, INT64_MAX, 0, NULL));

	xe_spin_end(spin[0]);
	xe_vm_unbind_sync(fd, vm[1], 0, addr2, bo_size);
	syncobj_destroy(fd, sync.handle);

	xe_exec_queue_destroy(fd, exec_queues[0]);
	xe_vm_destroy(fd, vm[0]);
	xe_exec_queue_destroy(fd, exec_queues[1]);
	xe_vm_destroy(fd, vm[1]);

	put_ahnd(ahnd[1]);
	put_ahnd(ahnd[0]);

	return elapsed;
}

static void test_timeout(int fd, int engine, const char **property, uint16_t class, int gt)
{
	uint64_t delays[] = { 1000, 50000, 100000, 500000 };
	unsigned int saved;
	uint64_t elapsed;
	uint64_t epsilon;

    /*
     * Send down some non-preemptable workloads and then request a
     * switch to a higher priority context. The HW will not be able to
     * respond, so the kernel will be forced to reset the hog. This
     * timeout should match our specification, and so we can measure
     * the delay from requesting the preemption to its completion.
     */

	igt_assert(igt_sysfs_scanf(engine, property[0], "%u", &saved) == 1);
	igt_debug("Initial %s:%u\n", property[0], saved);

	elapsed = __test_timeout(fd, engine, 1000, gt, class);
	epsilon = 2 * elapsed / 1000;
	if (epsilon < 50000)
		epsilon = 50000;
	igt_info("Minimum timeout measured as %.3fus; setting error threshold to %" PRId64 "us\n",
			 elapsed * 1e-3, epsilon);
	igt_require(epsilon < 10000000);

	for (int i = 0; i < ARRAY_SIZE(delays); i++) {
		elapsed = __test_timeout(fd, engine, delays[i], gt, class);
		igt_info("%s:%"PRId64", elapsed=%.3fus\n",
			property[0], delays[i], elapsed * 1e-3);

		/*
		 * We need to give a couple of jiffies slack for the scheduler
		 * timeouts and then a little more slack for the overhead in
		 * submitting and measuring.
		 */
			igt_assert_f(elapsed / 1000  < delays[i] + epsilon,
				 "Forced preemption timeout exceeded request!\n");
	}

	set_preempt_timeout(engine, saved);
}

#define	MAX_GTS	8
int igt_main()
{
	static const struct {
		const char *name;
		void (*fn)(int, int, const char **, uint16_t, int);
	} tests[] = {
		{ "timeout", test_timeout },
		{ }
	};
	const char *property[][3] = { {"preempt_timeout_us",
				       "preempt_timeout_min",
				       "preempt_timeout_max"}, };
	int count = sizeof(property) / sizeof(property[0]);
	int gt_count = 0;
	int fd = -1, sys_fd, gt;
	int engines_fd[MAX_GTS], gt_fd[MAX_GTS];
	unsigned int pts[MAX_GTS][XE_MAX_ENGINE_INSTANCE];
	int *engine_list[MAX_GTS];

	igt_fixture() {
		fd = drm_open_driver(DRIVER_XE);

		sys_fd = igt_sysfs_open(fd);
		igt_require(sys_fd != -1);
		close(sys_fd);

		xe_for_each_gt(fd, gt) {
			int *list, i = 0;

			igt_require(gt_count < MAX_GTS);

			gt_fd[gt_count] = xe_sysfs_gt_open(fd, gt);
			igt_require(gt_fd[gt_count] != -1);
			engines_fd[gt_count] = openat(gt_fd[gt_count], "engines", O_RDONLY);
			igt_require(engines_fd[gt_count] != -1);

			list = igt_sysfs_get_engine_list(engines_fd[gt_count]);

			while (list[i] != -1) {
				igt_require(igt_sysfs_scanf(list[i], "preempt_timeout_us", "%u",
							    &pts[gt_count][i]) == 1);
				i++;
			}

			igt_require(i > 0);
			engine_list[gt_count] = list;
			gt_count++;
		}
	}

	for (int i = 0; i < count; i++) {
		for (typeof(*tests) *t = tests; t->name; t++) {
			igt_subtest_with_dynamic_f("%s-%s", property[i][0], t->name) {
				int j = 0;
				xe_for_each_gt(fd, gt) {
					int e = engines_fd[j];

					igt_sysfs_engines(fd, e, gt, 1, property[i], t->fn);
					j++;
				}
			}
		}
	}
	igt_fixture() {
		for (int i = 0; i < gt_count; i++) {
			int *list, j = 0;

			list = engine_list[i];

			while (list[j] != -1) {
				unsigned int store = UINT_MAX;

				igt_sysfs_printf(list[j], "preempt_timeout_us",
						 "%u", pts[i][j]);
				igt_sysfs_scanf(list[j], "preempt_timeout_us",
						"%u", &store);
				igt_abort_on_f(store != pts[i][j],
					       "preempt_timeout_us not restored!\n");
				j++;
			}

			igt_sysfs_free_engine_list(list);
			close(engines_fd[i]);
			close(gt_fd[i]);
		}

		drm_close_driver(fd);
	}
}
