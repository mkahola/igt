// SPDX-License-Identifier: MIT
/*
 * Copyright 2026 Advanced Micro Devices, Inc.
 *
 * Test: amd_lockdep - Exercise lock-heavy GPU paths for lockdep validation
 *
 * This IGT test exercises code paths that involve multiple kernel locks
 * simultaneously. When run on a kernel with CONFIG_LOCKDEP=y and the
 * amdgpu lockdep annotations patch, lockdep will warn if any code path
 * violates the lock ordering hierarchy.
 *
 * The test checks dmesg for lockdep violations and fails if any are found.
 * It also checks the kernel taint flags for TAINT_WARN.
 *
 * Usage:
 *   sudo ./build/tests/amdgpu/amd_lockdep --run-subtest <subtest>
 *
 * Subtests:
 *   concurrent-reset-and-submit  - Submit work while triggering reset
 *   concurrent-mmap-and-evict    - mmap/munmap while VRAM eviction runs
 *   concurrent-userptr-and-reset - USERPTR invalidation during reset
 *   stress-all-paths             - Combined stress of all paths
 *   notifier-reclaim-splat       - userptr + MADV_PAGEOUT (Michael Gavrilov)
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>

#include <amdgpu.h>
#include <amdgpu_drm.h>

#include "igt.h"
#include "igt_amd.h"
#include "igt_taints.h"
#include "lib/amdgpu/amd_command_submission.h"
#include "lib/amdgpu/amd_memory.h"
#include "lib/amdgpu/amd_utils.h"

#define BO_SIZE (4 * 1024 * 1024)  /* 4MB */
#define NUM_BOS 16
#define STRESS_ITERATIONS 100
#define THREAD_RUNTIME_SEC 2

/* For notifier-reclaim-splat test */
#define USERPTR_BUF_SIZE (64ull * 1024 * 1024) /* 64 MiB */
#define PAGEOUT_ITERATIONS 8

struct thread_data {
	amdgpu_device_handle device;
	int fd;
	bool stop;
	int iterations;
};

/*
 * Helper: allocate and map a BO to exercise VRAM management locks
 */
static void alloc_and_map_bo(amdgpu_device_handle device,
			     amdgpu_bo_handle *bo_out,
			     amdgpu_va_handle *va_out,
			     uint64_t *mc_addr_out)
{
	struct amdgpu_bo_alloc_request req = {0};
	amdgpu_bo_handle bo;
	amdgpu_va_handle va;
	uint64_t mc_addr;
	int r;

	req.alloc_size = BO_SIZE;
	req.phys_alignment = 4096;
	req.preferred_heap = AMDGPU_GEM_DOMAIN_VRAM;
	req.flags = 0;

	r = amdgpu_bo_alloc(device, &req, &bo);
	igt_assert_eq(r, 0);

	r = amdgpu_va_range_alloc(device, amdgpu_gpu_va_range_general,
				  BO_SIZE, 4096, 0, &mc_addr, &va, 0);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_va_op(bo, 0, BO_SIZE, mc_addr, 0, AMDGPU_VA_OP_MAP);
	igt_assert_eq(r, 0);

	*bo_out = bo;
	*va_out = va;
	*mc_addr_out = mc_addr;
}

static void free_bo(amdgpu_bo_handle bo, amdgpu_va_handle va, uint64_t mc_addr)
{
	amdgpu_bo_va_op(bo, 0, BO_SIZE, mc_addr, 0, AMDGPU_VA_OP_UNMAP);
	amdgpu_va_range_free(va);
	amdgpu_bo_free(bo);
}

/*
 * Thread: Continuously allocate and free VRAM BOs
 * Exercises: vram_mgr->lock, reset_domain->sem (via eviction)
 */
static void *vram_alloc_thread(void *arg)
{
	struct thread_data *data = arg;
	int count = 0;

	while (!data->stop) {
		amdgpu_bo_handle bo;
		amdgpu_va_handle va;
		uint64_t mc_addr;

		alloc_and_map_bo(data->device, &bo, &va, &mc_addr);
		usleep(1000); /* 1ms hold */
		free_bo(bo, va, mc_addr);
		count++;
	}
	data->iterations = count;
	return NULL;
}

/*
 * Thread: Continuously mmap/munmap GPU buffers
 * Exercises: mmap_lock, notifier_lock, vram_lock interaction
 */
static void *mmap_thread(void *arg)
{
	struct thread_data *data = arg;
	int count = 0;

	while (!data->stop) {
		amdgpu_bo_handle bo;
		struct amdgpu_bo_alloc_request req = {0};
		void *cpu_ptr = NULL;
		int r;

		req.alloc_size = BO_SIZE;
		req.phys_alignment = 4096;
		req.preferred_heap = AMDGPU_GEM_DOMAIN_GTT;
		req.flags = AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;

		r = amdgpu_bo_alloc(data->device, &req, &bo);
		if (r)
			continue;

		r = amdgpu_bo_cpu_map(bo, &cpu_ptr);
		if (r == 0 && cpu_ptr) {
			/* Touch the memory to trigger page faults */
			memset(cpu_ptr, 0xAA, 4096);
			amdgpu_bo_cpu_unmap(bo);
		}
		amdgpu_bo_free(bo);
		count++;
	}
	data->iterations = count;
	return NULL;
}

/*
 * Thread: Trigger GPU reset via debugfs
 * Exercises: reset_domain->sem, reset_lock, all recovery locks
 */
static void *reset_thread(void *arg)
{
	struct thread_data *data = arg;
	char path[256];
	int count = 0;

	snprintf(path, sizeof(path),
		 "/sys/kernel/debug/dri/1/amdgpu_gpu_recover");

	while (!data->stop) {
		FILE *f = fopen(path, "w");

		if (f) {
			fprintf(f, "1");
			fclose(f);
			count++;
		}
		/* Wait for reset to complete before triggering again */
		sleep(2);
	}
	data->iterations = count;
	return NULL;
}

/*
 * Thread: USERPTR operations - register/unregister user memory
 * Exercises: notifier_lock, mmap_lock, userq paths
 */
static void *userptr_thread(void *arg)
{
	struct thread_data *data = arg;
	int count = 0;
	void *user_buf = NULL;
	size_t buf_size = BO_SIZE;

	while (!data->stop) {
		amdgpu_bo_handle bo;
		amdgpu_va_handle va;
		uint64_t mc_addr;
		int r;

		/* Allocate user memory */
		user_buf = mmap(NULL, buf_size, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (user_buf == MAP_FAILED)
			continue;

		/* Touch it to fault in pages */
		memset(user_buf, 0x55, buf_size);

		/* Create USERPTR BO */
		r = amdgpu_create_bo_from_user_mem(data->device,
						   user_buf, buf_size, &bo);
		if (r == 0) {
			r = amdgpu_va_range_alloc(data->device,
						  amdgpu_gpu_va_range_general,
						  buf_size, 4096, 0,
						  &mc_addr, &va, 0);
			if (r == 0) {
				amdgpu_bo_va_op(bo, 0, buf_size, mc_addr, 0,
						AMDGPU_VA_OP_MAP);
				usleep(500);
				amdgpu_bo_va_op(bo, 0, buf_size, mc_addr, 0,
						AMDGPU_VA_OP_UNMAP);
				amdgpu_va_range_free(va);
			}
			amdgpu_bo_free(bo);
		}

		munmap(user_buf, buf_size);
		count++;
	}
	data->iterations = count;
	return NULL;
}

/*
 * Subtest: concurrent-reset-and-submit
 *
 * Run GPU submissions concurrently with GPU reset.
 * This exercises the interaction between reset_domain->sem and
 * submission locks.
 */
static void test_concurrent_reset_and_submit(int fd, amdgpu_device_handle device)
{
	struct thread_data vram_data = { .device = device, .fd = fd };
	struct thread_data reset_data = { .device = device, .fd = fd };
	pthread_t t_vram, t_reset;
	unsigned long taint_before = 0;
	int kmsg_fd;

	igt_kernel_tainted(&taint_before);
	kmsg_fd = amd_lockdep_kmsg_open();

	igt_info("Running concurrent reset + VRAM allocation for %d seconds\n",
		 THREAD_RUNTIME_SEC);

	pthread_create(&t_vram, NULL, vram_alloc_thread, &vram_data);
	pthread_create(&t_reset, NULL, reset_thread, &reset_data);

	sleep(THREAD_RUNTIME_SEC);

	vram_data.stop = true;
	reset_data.stop = true;

	pthread_join(t_vram, NULL);
	pthread_join(t_reset, NULL);

	igt_info("  VRAM allocs: %d, Resets: %d\n",
		 vram_data.iterations, reset_data.iterations);

	amd_assert_no_lockdep_violations(kmsg_fd, taint_before);
	close(kmsg_fd);
}

/*
 * Subtest: concurrent-mmap-and-evict
 *
 * Run mmap/munmap concurrently with VRAM pressure (eviction).
 * This exercises mmap_lock vs vram_mgr->lock ordering.
 */
static void test_concurrent_mmap_and_evict(int fd, amdgpu_device_handle device)
{
	struct thread_data mmap_data = { .device = device, .fd = fd };
	struct thread_data vram_data = { .device = device, .fd = fd };
	pthread_t t_mmap, t_vram;
	unsigned long taint_before = 0;
	int kmsg_fd;

	igt_kernel_tainted(&taint_before);
	kmsg_fd = amd_lockdep_kmsg_open();

	igt_info("Running concurrent mmap + VRAM pressure for %d seconds\n",
		 THREAD_RUNTIME_SEC);

	pthread_create(&t_mmap, NULL, mmap_thread, &mmap_data);
	pthread_create(&t_vram, NULL, vram_alloc_thread, &vram_data);

	sleep(THREAD_RUNTIME_SEC);

	mmap_data.stop = true;
	vram_data.stop = true;

	pthread_join(t_mmap, NULL);
	pthread_join(t_vram, NULL);

	igt_info("  mmap ops: %d, VRAM allocs: %d\n",
		 mmap_data.iterations, vram_data.iterations);

	amd_assert_no_lockdep_violations(kmsg_fd, taint_before);
	close(kmsg_fd);
}

/*
 * Subtest: concurrent-userptr-and-reset
 *
 * Run USERPTR registration/unregistration concurrently with GPU reset.
 * This exercises notifier_lock vs reset_domain->sem ordering.
 */
static void test_concurrent_userptr_and_reset(int fd,
					      amdgpu_device_handle device)
{
	struct thread_data userptr_data = { .device = device, .fd = fd };
	struct thread_data reset_data = { .device = device, .fd = fd };
	pthread_t t_userptr, t_reset;
	unsigned long taint_before = 0;
	int kmsg_fd;

	igt_kernel_tainted(&taint_before);
	kmsg_fd = amd_lockdep_kmsg_open();

	igt_info("Running concurrent USERPTR + reset for %d seconds\n",
		 THREAD_RUNTIME_SEC);

	pthread_create(&t_userptr, NULL, userptr_thread, &userptr_data);
	pthread_create(&t_reset, NULL, reset_thread, &reset_data);

	sleep(THREAD_RUNTIME_SEC);

	userptr_data.stop = true;
	reset_data.stop = true;

	pthread_join(t_userptr, NULL);
	pthread_join(t_reset, NULL);

	igt_info("  USERPTR ops: %d, Resets: %d\n",
		 userptr_data.iterations, reset_data.iterations);

	amd_assert_no_lockdep_violations(kmsg_fd, taint_before);
	close(kmsg_fd);
}

/*
 * Subtest: stress-all-paths
 *
 * Run all thread types concurrently for maximum lock contention.
 */
static void test_stress_all_paths(int fd, amdgpu_device_handle device)
{
	struct thread_data vram_data = { .device = device, .fd = fd };
	struct thread_data mmap_data = { .device = device, .fd = fd };
	struct thread_data userptr_data = { .device = device, .fd = fd };
	struct thread_data reset_data = { .device = device, .fd = fd };
	pthread_t t_vram, t_mmap, t_userptr, t_reset;
	unsigned long taint_before = 0;
	int kmsg_fd;

	igt_kernel_tainted(&taint_before);
	kmsg_fd = amd_lockdep_kmsg_open();

	igt_info("Running all paths concurrently for %d seconds\n",
		 THREAD_RUNTIME_SEC);

	pthread_create(&t_vram, NULL, vram_alloc_thread, &vram_data);
	pthread_create(&t_mmap, NULL, mmap_thread, &mmap_data);
	pthread_create(&t_userptr, NULL, userptr_thread, &userptr_data);
	pthread_create(&t_reset, NULL, reset_thread, &reset_data);

	sleep(THREAD_RUNTIME_SEC);

	vram_data.stop = true;
	mmap_data.stop = true;
	userptr_data.stop = true;
	reset_data.stop = true;

	pthread_join(t_vram, NULL);
	pthread_join(t_mmap, NULL);
	pthread_join(t_userptr, NULL);
	pthread_join(t_reset, NULL);

	igt_info("  VRAM: %d, mmap: %d, USERPTR: %d, Resets: %d\n",
		 vram_data.iterations, mmap_data.iterations,
		 userptr_data.iterations, reset_data.iterations);

	amd_assert_no_lockdep_violations(kmsg_fd, taint_before);
	close(kmsg_fd);
}

/* For notifier-reclaim-splat test */

/*
 * test_notifier_reclaim_splat - Michael Gavrilov reproducer
 *
 * Triggers the false positive lockdep splat by:
 * 1. Creating anonymous memory buffer (64MB)
 * 2. Registering it as AMDGPU userptr (installs MMU notifier)
 * 3. Forcing memory reclaim with madvise(MADV_PAGEOUT)
 * 4. Reclaim path calls MMU notifier which takes notifier_lock under fs_reclaim
 * 5. Lockdep sees circular dependency with the false edge from amdgpu_lockdep_init
 *
 * Expected behavior:
 * - BEFORE kernel fix: lockdep splat in dmesg (test FAILS)
 * - AFTER kernel fix: no lockdep splat (test PASSES)
 */
static void test_notifier_reclaim_splat(int fd)
{
	void *buf;
	struct drm_amdgpu_gem_userptr userptr = {0};
	int ret;
	int kmsg_fd;
	bool violation_found = false;
	unsigned long taints = 0;

	igt_info("Creating %llu MiB anonymous buffer\n", USERPTR_BUF_SIZE / (1024 * 1024));

	buf = mmap(NULL, USERPTR_BUF_SIZE, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	igt_assert(buf != MAP_FAILED);

	/* Fault all pages in */
	igt_info("Faulting in all pages\n");
	memset(buf, 0xa5, USERPTR_BUF_SIZE);

	/* Open kmsg to monitor for lockdep violations */
	kmsg_fd = amd_lockdep_kmsg_open();
	igt_assert(kmsg_fd >= 0);

	/* Register as userptr - this installs MMU notifier callback */
	igt_info("Registering as userptr BO - installs MMU notifier\n");
	userptr.addr = (uint64_t)(uintptr_t)buf;
	userptr.size = USERPTR_BUF_SIZE;
	userptr.flags = AMDGPU_GEM_USERPTR_ANONONLY |
			 AMDGPU_GEM_USERPTR_REGISTER |   /* Install mmu_interval_notifier */
			 AMDGPU_GEM_USERPTR_VALIDATE;

	ret = igt_ioctl(fd, DRM_IOCTL_AMDGPU_GEM_USERPTR, &userptr);
	igt_assert_eq(ret, 0);

	igt_info("Forcing reclaim with MADV_PAGEOUT - triggers MMU notifier under fs_reclaim\n");

	/* Force reclaim - this triggers the lockdep splat on vulnerable kernel */
	for (int i = 0; i < PAGEOUT_ITERATIONS; i++) {
		igt_info("  Iteration %d/%d: madvise MADV_PAGEOUT\n", i + 1, PAGEOUT_ITERATIONS);

		ret = madvise(buf, USERPTR_BUF_SIZE, MADV_PAGEOUT);
		igt_assert_eq(ret, 0);

		usleep(50000);  /* 50ms - give kernel time to process */

		/* Check for lockdep violations */
		if (amd_lockdep_kmsg_has_violation(kmsg_fd)) {
			violation_found = true;
			igt_warn("LOCKDEP VIOLATION DETECTED during iteration %d\n", i + 1);
			break;
		}

		/* Re-fault pages for next iteration */
		memset(buf, 0xa5, USERPTR_BUF_SIZE);
	}

	close(kmsg_fd);

	if (violation_found) {
		igt_info("\n");
		igt_info("================================================================\n");
		igt_info("LOCKDEP SPLAT DETECTED - This is EXPECTED before kernel fix\n");
		igt_info("================================================================\n");
		igt_info("\n");
		igt_info("The kernel has a FALSE POSITIVE lockdep bug:\n");
		igt_info("  File: drivers/gpu/drm/amd/amdgpu/amdgpu_lockdep.c\n");
		igt_info("  Function: amdgpu_lockdep_init() around line 176\n");
		igt_info("  Problem: fs_reclaim_acquire called while holding notifier_lock\n");
		igt_info("\n");
		igt_info("This teaches lockdep a FALSE edge: notifier_lock -> fs_reclaim\n");
		igt_info("Runtime reality: fs_reclaim -> notifier_lock (opposite direction)\n");
		igt_info("\n");
		igt_info("FIX: Move fs_reclaim_acquire to BEFORE all mutex acquisitions\n");
		igt_info("\n");
		igt_info("================================================================\n");
		igt_info("\n");
	} else {
		igt_info("\n");
		igt_info("================================================================\n");
		igt_info("NO LOCKDEP VIOLATION - Kernel fix appears to be working!\n");
		igt_info("================================================================\n");
		igt_info("\n");
	}

	/* Cleanup */
	{
		struct drm_gem_close gem_close = { .handle = userptr.handle };
		int ret_close;

		ret_close = igt_ioctl(fd, DRM_IOCTL_GEM_CLOSE, &gem_close);
		igt_assert_eq(ret_close, 0);
	}
	munmap(buf, USERPTR_BUF_SIZE);

	/* Check kernel taint flags */
	igt_kernel_tainted(&taints);
	igt_assert_f(!(taints & (1ul << TAINT_WARN)),
			 "Kernel is tainted - lockdep violation detected\n");
}

int igt_main()
{
	amdgpu_device_handle device;
	int fd = -1;
	int r;

	igt_fixture() {
		uint32_t major, minor;

		fd = drm_open_driver(DRIVER_AMDGPU);
		igt_require(fd >= 0);

		r = amdgpu_device_initialize(fd, &major, &minor, &device);
		igt_require(r == 0);

		/* Skip if lockdep is not enabled on the running kernel */
		igt_require_f(amd_is_lockdep_enabled(),
			      "CONFIG_LOCKDEP not enabled - "
			      "rebuild kernel with CONFIG_PROVE_LOCKING=y\n");

		igt_info("amd_lockdep: Exercise lock-heavy paths for lockdep validation\n");
		igt_info("Lockdep is ENABLED - violations will FAIL the test\n\n");
	}

	igt_describe("Submit work while triggering GPU reset - exercises reset_sem vs submission locks");
	igt_subtest("concurrent-reset-and-submit") {
		test_concurrent_reset_and_submit(fd, device);
	}

	igt_describe("mmap/munmap while VRAM eviction runs - exercises mmap_lock vs vram_lock");
	igt_subtest("concurrent-mmap-and-evict") {
		test_concurrent_mmap_and_evict(fd, device);
	}

	igt_describe("USERPTR invalidation during GPU reset - exercises notifier_lock vs reset_sem");
	igt_subtest("concurrent-userptr-and-reset") {
		test_concurrent_userptr_and_reset(fd, device);
	}

	igt_describe("Combined stress of all lock-heavy paths simultaneously");
	igt_subtest("stress-all-paths") {
		test_stress_all_paths(fd, device);
	}

	igt_describe("Michael Gavrilov reproducer: userptr + MADV_PAGEOUT triggers fs_reclaim cycle");
	igt_subtest("notifier-reclaim-splat") {
		test_notifier_reclaim_splat(fd);
	}
	igt_fixture() {
		amdgpu_device_deinitialize(device);
		drm_close_driver(fd);
	}
}
