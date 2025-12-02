// SPDX-License-Identifier: MIT
/*
 * Copyright 2025 Advanced Micro Devices, Inc.
 */

#include "lib/amdgpu/amd_memory.h"
#include "lib/amdgpu/amd_sdma.h"
#include "lib/amdgpu/amd_PM4.h"
#include "lib/amdgpu/amd_command_submission.h"
#include "lib/amdgpu/amd_compute.h"
#include "lib/amdgpu/amd_gfx.h"
#include "lib/amdgpu/shaders/amd_shaders.h"
#include "lib/amdgpu/compute_utils/amd_dispatch.h"
#include "igt.h"
#include "igt_multigpu.h"

#define BUFFER_SIZE (8 * 1024)

/**
 * MEM ALLOC TEST
 * @param device
 */
static void amdgpu_memory_alloc(amdgpu_device_handle device)
{
	amdgpu_bo_handle bo;
	amdgpu_va_handle va_handle;
	uint64_t bo_mc;

	/* Test visible VRAM */
	bo = gpu_mem_alloc(device,
			   4096, 4096,
			   AMDGPU_GEM_DOMAIN_VRAM,
			   AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED,
			   &bo_mc, &va_handle);

	gpu_mem_free(bo, va_handle, bo_mc, 4096);

	/* Test invisible VRAM */
	bo = gpu_mem_alloc(device,
			   4096, 4096,
			   AMDGPU_GEM_DOMAIN_VRAM,
			   AMDGPU_GEM_CREATE_NO_CPU_ACCESS,
			   &bo_mc, &va_handle);

	gpu_mem_free(bo, va_handle, bo_mc, 4096);

	/* Test GART Cacheable */
	bo = gpu_mem_alloc(device,
			   4096, 4096,
			   AMDGPU_GEM_DOMAIN_GTT,
			   0, &bo_mc, &va_handle);

	gpu_mem_free(bo, va_handle, bo_mc, 4096);

	/* Test GART USWC */
	bo = gpu_mem_alloc(device,
			   4096, 4096,
			   AMDGPU_GEM_DOMAIN_GTT,
			   AMDGPU_GEM_CREATE_CPU_GTT_USWC,
			   &bo_mc, &va_handle);

	gpu_mem_free(bo, va_handle, bo_mc, 4096);
}


/**
 * AMDGPU_HW_IP_GFX
 * @param device
 */
static void amdgpu_command_submission_gfx(amdgpu_device_handle device,
					  bool ce_avails,
					  bool user_queue)
{

	/* write data using the CP */
	amdgpu_command_submission_write_linear_helper(device,
						      get_ip_block(device, AMDGPU_HW_IP_GFX),
						      false, user_queue);

	/* const fill using the CP */
	amdgpu_command_submission_const_fill_helper(device,
						    get_ip_block(device, AMDGPU_HW_IP_GFX),
						    user_queue);

	/* copy data using the CP */
	amdgpu_command_submission_copy_linear_helper(device,
						     get_ip_block(device, AMDGPU_HW_IP_GFX),
						     user_queue);
	if (ce_avails) {
		/* separate IB buffers for multi-IB submission */
		amdgpu_command_submission_gfx_separate_ibs(device);
		/* shared IB buffer for multi-IB submission */
		amdgpu_command_submission_gfx_shared_ib(device);
	} else {
		igt_info("separate and shared IB buffers for multi IB submisison testes are skipped due to GFX11\n");
	}
}

/**
 * AMDGPU_HW_IP_COMPUTE
 * @param device
 */
static void amdgpu_command_submission_compute(amdgpu_device_handle device, bool user_queue)
{
	/* write data using the CP */
	amdgpu_command_submission_write_linear_helper(device,
						      get_ip_block(device, AMDGPU_HW_IP_COMPUTE),
						      false, user_queue);
	/* const fill using the CP */
	amdgpu_command_submission_const_fill_helper(device,
						    get_ip_block(device, AMDGPU_HW_IP_COMPUTE),
						    user_queue);
	/* copy data using the CP */
	amdgpu_command_submission_copy_linear_helper(device,
						     get_ip_block(device, AMDGPU_HW_IP_COMPUTE),
						     user_queue);
	/* nop test */
	amdgpu_command_submission_nop(device, AMDGPU_HW_IP_COMPUTE, user_queue);
}

/**
 * AMDGPU_HW_IP_DMA
 * @param device
 */
static void amdgpu_command_submission_sdma(amdgpu_device_handle device, bool user_queue)
{
	amdgpu_command_submission_write_linear_helper(device,
						      get_ip_block(device, AMDGPU_HW_IP_DMA),
						      false, user_queue);

	amdgpu_command_submission_const_fill_helper(device,
						    get_ip_block(device, AMDGPU_HW_IP_DMA),
						    user_queue);

	amdgpu_command_submission_copy_linear_helper(device,
						     get_ip_block(device, AMDGPU_HW_IP_DMA),
						     user_queue);
	/* nop test */
	amdgpu_command_submission_nop(device, AMDGPU_HW_IP_DMA, user_queue);
}

static void amdgpu_test_all_queues(amdgpu_device_handle device, bool user_queue)
{
	amdgpu_command_submission_write_linear_helper2(device, AMDGPU_HW_IP_GFX, false, user_queue);
	amdgpu_command_submission_write_linear_helper2(device, AMDGPU_HW_IP_COMPUTE, false, user_queue);
	amdgpu_command_submission_write_linear_helper2(device, AMDGPU_HW_IP_DMA, false, user_queue);
	amdgpu_command_submission_write_linear_helper2(device, AMDGPU_HW_IP_GFX | AMDGPU_HW_IP_COMPUTE |
							AMDGPU_HW_IP_DMA, false, user_queue);
}
/**
 * MULTI FENCE
 * @param device
 */
static void amdgpu_command_submission_multi_fence(amdgpu_device_handle device)
{
	amdgpu_command_submission_multi_fence_wait_all(device, true);
	amdgpu_command_submission_multi_fence_wait_all(device, false);
}

igt_main
{
	amdgpu_device_handle device;
	int fd = -1;
	bool enable_test = false;
#ifdef AMDGPU_USERQ_ENABLED
	const char *env = getenv("AMDGPU_ENABLE_USERQTEST");

	enable_test = env && atoi(env);
#endif

	igt_fixture {
		uint32_t major, minor;
		int err;

		fd = drm_open_driver(DRIVER_AMDGPU);

		err = amdgpu_device_initialize(fd, &major, &minor, &device);
		igt_require(err == 0);

		igt_info("Initialized amdgpu, driver version %d.%d\n",
			 major, minor);
	}

	igt_subtest("multi-gpu-memeory-alloc") {
		igt_multi_fork_foreach_gpu(gpu_fd, gpu_idx, DRIVER_AMDGPU) {
			amdgpu_device_handle dev;
			uint32_t major, minor;
			int res;

			/* Initialize device */
			res = amdgpu_device_initialize(gpu_fd, &major, &minor, &dev);
			igt_assert_eq(res, 0);

			/* Run test */
			amdgpu_memory_alloc(dev);

			amdgpu_device_deinitialize(dev);
		}
		igt_waitchildren();
	}

	igt_subtest("cs-gfx-with-IP-GFX") {
		igt_multi_fork_foreach_gpu(gpu_fd, gpu_idx, DRIVER_AMDGPU) {
			amdgpu_device_handle dev;
			uint32_t major, minor;
			struct amdgpu_gpu_info gpus_info = {0};
			bool arr_caps[AMD_IP_MAX] = {0};
			struct drm_amdgpu_info_hw_ip info = {0};
			int res;

			/* Initialize device */
			res = amdgpu_device_initialize(gpu_fd, &major, &minor, &dev);
			igt_assert_eq(res, 0);

			res = amdgpu_query_gpu_info(dev, &gpus_info);
			igt_assert_eq(res, 0);
			res = amdgpu_query_hw_ip_info(device, AMDGPU_HW_IP_GFX, 0, &info);
			igt_assert_eq(res, 0);
			res = setup_amdgpu_ip_blocks(major, minor, &gpus_info, dev);
			igt_assert_eq(res, 0);
			asic_rings_readness(dev, 1, arr_caps);
			/* Run test */
			if (arr_caps[AMD_IP_GFX])
				amdgpu_command_submission_gfx(dev, info.hw_ip_version_major < 11, false);
			amdgpu_device_deinitialize(dev);
		}
		igt_waitchildren();
	}

	igt_subtest("cs-compute-with-IP-COMPUTE") {
		igt_multi_fork_foreach_gpu(gpu_fd, gpu_idx, DRIVER_AMDGPU) {
			amdgpu_device_handle dev;
			uint32_t major, minor;
			struct amdgpu_gpu_info gpus_info = {0};
			bool arr_caps[AMD_IP_MAX] = {0};
			int res;

			/* Initialize device */
			res = amdgpu_device_initialize(gpu_fd, &major, &minor, &dev);
			igt_assert_eq(res, 0);

			res = amdgpu_query_gpu_info(dev, &gpus_info);
			igt_assert_eq(res, 0);

			res = setup_amdgpu_ip_blocks(major, minor, &gpus_info, dev);
			igt_assert_eq(res, 0);
			asic_rings_readness(dev, 1, arr_caps);
			/* Run test */
			if (arr_caps[AMD_IP_COMPUTE])
				amdgpu_command_submission_compute(dev, false);
			amdgpu_device_deinitialize(dev);
		}
		igt_waitchildren();
	}

	igt_subtest("cs-multi-fence-with-IP-GFX") {
		igt_multi_fork_foreach_gpu(gpu_fd, gpu_idx, DRIVER_AMDGPU) {
			amdgpu_device_handle dev;
			uint32_t major, minor;
			struct amdgpu_gpu_info gpus_info = {0};
			struct drm_amdgpu_info_hw_ip info = {0};
			bool arr_caps[AMD_IP_MAX] = {0};
			int res;

			/* Initialize device */
			res = amdgpu_device_initialize(gpu_fd, &major, &minor, &dev);
			igt_assert_eq(res, 0);

			res = amdgpu_query_gpu_info(dev, &gpus_info);
			igt_assert_eq(res, 0);
			res = amdgpu_query_hw_ip_info(device, AMDGPU_HW_IP_GFX, 0, &info);
			igt_assert_eq(res, 0);
			res = setup_amdgpu_ip_blocks(major, minor, &gpus_info, dev);
			igt_assert_eq(res, 0);
			asic_rings_readness(dev, 1, arr_caps);
			/* Run test */
			if (arr_caps[AMD_IP_GFX] && info.hw_ip_version_major < 11) {
				amdgpu_command_submission_multi_fence(dev);
			} else {
				igt_info("cs-multi-fence-with-IP-GFX testes are skipped due to GFX11 or no GFX_IP\n");
			}
			amdgpu_device_deinitialize(dev);
		}
		igt_waitchildren();
	}

	igt_subtest("cs-sdma-with-IP-DMA") {
		igt_multi_fork_foreach_gpu(gpu_fd, gpu_idx, DRIVER_AMDGPU) {
			amdgpu_device_handle dev;
			uint32_t major, minor;
			struct amdgpu_gpu_info gpus_info = {0};
			bool arr_caps[AMD_IP_MAX] = {0};
			int res;

			/* Initialize device */
			res = amdgpu_device_initialize(gpu_fd, &major, &minor, &dev);
			igt_assert_eq(res, 0);

			res = amdgpu_query_gpu_info(dev, &gpus_info);
			igt_assert_eq(res, 0);

			res = setup_amdgpu_ip_blocks(major, minor, &gpus_info, dev);
			igt_assert_eq(res, 0);
			asic_rings_readness(dev, 1, arr_caps);
			/* Run test */
			if (arr_caps[AMD_IP_DMA])
				amdgpu_command_submission_sdma(dev, false);
			amdgpu_device_deinitialize(dev);
		}
		igt_waitchildren();
	}

	igt_subtest("multi-gpu-cs-gfx-with-IP-GFX-UMQ") {
		igt_multi_fork_foreach_gpu(gpu_fd, gpu_idx, DRIVER_AMDGPU) {
			amdgpu_device_handle dev;
			uint32_t major, minor;
			struct amdgpu_gpu_info gpus_info = {0};
			bool userq_arr_caps[AMD_IP_MAX] = {0};
			int res;

			/* Initialize device */
			res = amdgpu_device_initialize(gpu_fd, &major, &minor, &dev);
			igt_assert_eq(res, 0);

			res = amdgpu_query_gpu_info(dev, &gpus_info);
			igt_assert_eq(res, 0);

			res = setup_amdgpu_ip_blocks(major, minor, &gpus_info, dev);
			igt_assert_eq(res, 0);
			asic_userq_readiness(device, userq_arr_caps);
			/* Run test */
			if (enable_test && userq_arr_caps[AMD_IP_GFX])
				amdgpu_command_submission_gfx(dev, false, true);
			amdgpu_device_deinitialize(dev);
		}
		igt_waitchildren();
	}

	igt_subtest("multi-gpu-cs-compute-with-IP-COMPUTE-UMQ") {
		igt_multi_fork_foreach_gpu(gpu_fd, gpu_idx, DRIVER_AMDGPU) {
			amdgpu_device_handle dev;
			uint32_t major, minor;
			struct amdgpu_gpu_info gpus_info = {0};
			bool userq_arr_caps[AMD_IP_MAX] = {0};
			int res;

			res = amdgpu_device_initialize(gpu_fd, &major, &minor, &dev);
			igt_assert_eq(res, 0);

			res = amdgpu_query_gpu_info(dev, &gpus_info);
			igt_assert_eq(res, 0);

			res = setup_amdgpu_ip_blocks(major, minor, &gpus_info, dev);
			igt_assert_eq(res, 0);
			asic_userq_readiness(device, userq_arr_caps);
			/* Run test */
			if (enable_test && userq_arr_caps[AMD_IP_COMPUTE])
				amdgpu_command_submission_compute(dev, true);
			amdgpu_device_deinitialize(dev);
		}
		igt_waitchildren();
	}

	igt_subtest("multi-gpu-cs-sdma-with-IP-DMA-UMQ") {
		igt_multi_fork_foreach_gpu(gpu_fd, gpu_idx, DRIVER_AMDGPU) {
			amdgpu_device_handle dev;
			uint32_t major, minor;
			struct amdgpu_gpu_info gpus_info = {0};
			bool userq_arr_caps[AMD_IP_MAX] = {0};
			int res;

			/* Initialize device */
			res = amdgpu_device_initialize(gpu_fd, &major, &minor, &dev);
			igt_assert_eq(res, 0);

			res = amdgpu_query_gpu_info(dev, &gpus_info);
			igt_assert_eq(res, 0);

			res = setup_amdgpu_ip_blocks(major, minor, &gpus_info, dev);
			igt_assert_eq(res, 0);
			asic_userq_readiness(dev, userq_arr_caps);
			/* Run test */
			if (enable_test && userq_arr_caps[AMD_IP_DMA])
				amdgpu_command_submission_sdma(dev, true);
			amdgpu_device_deinitialize(dev);
		}
		igt_waitchildren();
	}

	igt_subtest("multi-gpu-all-queues-test-with-UMQ") {
		igt_multi_fork_foreach_gpu(gpu_fd, gpu_idx, DRIVER_AMDGPU) {
			amdgpu_device_handle dev;
			uint32_t major, minor;
			struct amdgpu_gpu_info gpus_info = {0};
			bool userq_arr_caps[AMD_IP_MAX] = {0};
			int res;

			/* Initialize device */
			res = amdgpu_device_initialize(gpu_fd, &major, &minor, &dev);
			igt_assert_eq(res, 0);

			res = amdgpu_query_gpu_info(dev, &gpus_info);
			igt_assert_eq(res, 0);

			res = setup_amdgpu_ip_blocks(major, minor, &gpus_info, dev);
			igt_assert_eq(res, 0);
			asic_userq_readiness(dev, userq_arr_caps);
			/* Run test */
			if (enable_test && userq_arr_caps[AMD_IP_GFX] &&
				userq_arr_caps[AMD_IP_COMPUTE] &&
				userq_arr_caps[AMD_IP_DMA])
			amdgpu_test_all_queues(dev, true);

			amdgpu_device_deinitialize(dev);
		}
		igt_waitchildren();
	}

	igt_fixture {
		amdgpu_device_deinitialize(device);
		drm_close_driver(fd);
	}
}
