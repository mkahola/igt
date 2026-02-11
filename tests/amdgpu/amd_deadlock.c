// SPDX-License-Identifier: MIT
/*
 * Copyright 2014 Advanced Micro Devices, Inc.
 * Copyright 2022 Advanced Micro Devices, Inc.
 * Copyright 2023 Advanced Micro Devices, Inc.
 */

#include "lib/amdgpu/amd_memory.h"
#include "lib/amdgpu/amd_command_submission.h"
#include "lib/amdgpu/amd_deadlock_helpers.h"
#include "lib/amdgpu/amdgpu_asic_addr.h"
#include "lib/amdgpu/amd_utils.h"

#define AMDGPU_FAMILY_SI                        110 /* Hainan, Oland, Verde, Pitcairn, Tahiti */
#define AMDGPU_FAMILY_CI                        120 /* Bonaire, Hawaii */
#define AMDGPU_FAMILY_CZ                        135 /* Carrizo, Stoney */
#define AMDGPU_FAMILY_RV                        142 /* Raven */

static bool
is_deadlock_tests_enable(const struct amdgpu_gpu_info *gpu_info)
{
	bool enable = true;
	/*
	 * skip for the ASICs that don't support GPU reset.
	 */
	if (gpu_info->family_id == AMDGPU_FAMILY_SI ||
	    gpu_info->family_id == AMDGPU_FAMILY_KV ||
	    gpu_info->family_id == AMDGPU_FAMILY_CZ ||
	    ((gpu_info->family_id == AMDGPU_FAMILY_RV) &&
	     (!ASICREV_IS_RENOIR(gpu_info->chip_external_rev)))) {
		igt_info("\n\nGPU reset is not enabled for the ASIC, deadlock test skip\n");
		enable = false;
	}
	return enable;
}

int igt_main()
{
	amdgpu_device_handle device;
	struct amdgpu_gpu_info gpu_info = {0};
	int fd = -1;
	int r;
	bool arr_cap[AMD_IP_MAX] = {0};
	bool userq_arr_cap[AMD_IP_MAX] = {0};
	struct pci_addr pci;

#ifdef AMDGPU_USERQ_ENABLED
	bool enable_test;
	const char *env = getenv("AMDGPU_ENABLE_USERQTEST");

	enable_test = env && atoi(env);
#endif

	igt_fixture() {
		uint32_t major, minor;
		int err;

		log_total_time(true, igt_test_name());
		fd = drm_open_driver(DRIVER_AMDGPU);

		err = amdgpu_device_initialize(fd, &major, &minor, &device);
		igt_require(err == 0);

		igt_info("Initialized amdgpu, driver version %d.%d\n",
			 major, minor);

		r = amdgpu_query_gpu_info(device, &gpu_info);
		igt_assert_eq(r, 0);
		r = setup_amdgpu_ip_blocks(major, minor, &gpu_info, device);
		igt_assert_eq(r, 0);
		asic_rings_readness(device, 1, arr_cap);
		asic_userq_readiness(device, userq_arr_cap);
		igt_skip_on(!is_deadlock_tests_enable(&gpu_info));

		igt_skip_on(get_pci_addr_from_fd(fd, &pci));
		igt_info("PCI Address: domain %04x, bus %02x, device %02x, function %02x\n",
				pci.domain, pci.bus, pci.device, pci.function);
	}
	igt_describe("Test-GPU-reset-by-flooding-sdma-ring-with-jobs");
	igt_subtest_with_dynamic("amdgpu-deadlock-sdma") {
		if (arr_cap[AMD_IP_DMA]) {
			igt_dynamic_f("amdgpu-deadlock-sdma")
			amdgpu_wait_memory_helper(device, AMDGPU_HW_IP_DMA, &pci, false);
		}
	}

	igt_describe("Test-GPU-reset-by-access-gfx-illegal-reg");
	igt_subtest_with_dynamic("amdgpu-gfx-illegal-reg-access") {
		if (arr_cap[AMD_IP_GFX] &&
			is_reset_enable(AMD_IP_GFX, AMDGPU_RESET_TYPE_PER_QUEUE, &pci)) {
			igt_dynamic_f("amdgpu-illegal-reg-access")
			bad_access_ring_helper(device, CMD_STREAM_TRANS_BAD_REG_ADDRESS,
					AMDGPU_HW_IP_GFX, &pci, false);
		}
	}

	igt_describe("Test-GPU-reset-by-access-gfx-illegal-mem-addr");
	igt_subtest_with_dynamic("amdgpu-gfx-illegal-mem-access") {
		if (arr_cap[AMD_IP_GFX] &&
			is_reset_enable(AMD_IP_GFX, AMDGPU_RESET_TYPE_PER_QUEUE, &pci)) {
			igt_dynamic_f("amdgpu-illegal-mem-access")
			bad_access_ring_helper(device, CMD_STREAM_TRANS_BAD_MEM_ADDRESS,
					AMDGPU_HW_IP_GFX, &pci, false);
		}
	}


	igt_describe("Test-GPU-reset-by-flooding-gfx-ring-with-jobs");
	igt_subtest_with_dynamic("amdgpu-deadlock-gfx") {
		if (arr_cap[AMD_IP_GFX]) {
			igt_dynamic_f("amdgpu-deadlock-gfx")
			amdgpu_wait_memory_helper(device, AMDGPU_HW_IP_GFX, &pci, false);
		}
	}

	igt_describe("Test-GPU-reset-by-access-compute-illegal-mem-addr");
	igt_subtest("amdgpu-compute-illegal-mem-access") {
		if (arr_cap[AMD_IP_COMPUTE] &&
			 is_reset_enable(AMD_IP_COMPUTE, AMDGPU_RESET_TYPE_PER_QUEUE, &pci)) {
		bad_access_ring_helper(device, CMD_STREAM_TRANS_BAD_MEM_ADDRESS,
				AMDGPU_HW_IP_COMPUTE, &pci, false);
		}
	}

	igt_describe("Test-GPU-reset-by-flooding-compute-ring-with-jobs");
	igt_subtest_with_dynamic("amdgpu-deadlock-compute") {
		if (arr_cap[AMD_IP_COMPUTE]) {
			igt_dynamic_f("amdgpu-deadlock-compute")
			amdgpu_wait_memory_helper(device, AMDGPU_HW_IP_COMPUTE, &pci, false);
		}
	}

	igt_describe("Test-GPU-reset-by-sdma-corrupted-header-with-jobs");
	igt_subtest_with_dynamic("amdgpu-deadlock-sdma-corrupted-header-test") {
		if (arr_cap[AMD_IP_DMA] &&
			is_reset_enable(AMD_IP_DMA, AMDGPU_RESET_TYPE_PER_QUEUE, &pci)) {
			igt_dynamic_f("amdgpu-deadlock-sdma-corrupted-header-test")
			amdgpu_hang_sdma_ring_helper(device, DMA_CORRUPTED_HEADER_HANG, &pci);
		}
	}

	igt_describe("Test-GPU-reset-by-sdma-slow-linear-copy-with-jobs");
	igt_subtest_with_dynamic("amdgpu-deadlock-sdma-slow-linear-copy") {
		if (arr_cap[AMD_IP_DMA] &&
			is_reset_enable(AMD_IP_DMA, AMDGPU_RESET_TYPE_PER_QUEUE, &pci)) {
			igt_dynamic_f("amdgpu-deadlock-sdma-slow-linear-copy")
			amdgpu_hang_sdma_ring_helper(device, DMA_SLOW_LINEARCOPY_HANG, &pci);
		}
	}

	igt_describe("Test-GPU-reset-by-sdma-badop-with-jobs");
	igt_subtest_with_dynamic("amdgpu-deadlock-sdma-badop-test") {
		if (arr_cap[AMD_IP_DMA] &&
			is_reset_enable(AMD_IP_DMA, AMDGPU_RESET_TYPE_PER_QUEUE, &pci)) {
			igt_dynamic_f("amdgpu-deadlock-sdma-badop-test")
			bad_access_ring_helper(device, CMD_STREAM_EXEC_INVALID_OPCODE,
					AMDGPU_HW_IP_DMA, &pci, false);
		}
	}

	igt_describe("Test-GPU-reset-by-sdma-bad-mem-with-jobs");
	igt_subtest_with_dynamic("amdgpu-deadlock-sdma-bad-mem-test") {
		if (arr_cap[AMD_IP_DMA] &&
			is_reset_enable(AMD_IP_DMA, AMDGPU_RESET_TYPE_PER_QUEUE, &pci)) {
			igt_dynamic_f("amdgpu-deadlock-sdma-bad-mem-test")
			bad_access_ring_helper(device, CMD_STREAM_TRANS_BAD_MEM_ADDRESS,
					AMDGPU_HW_IP_DMA, &pci, false);
		}
	}

	igt_describe("Test-GPU-reset-by-sdma-bad-reg-with-jobs");
	igt_subtest_with_dynamic("amdgpu-deadlock-sdma-bad-reg-test") {
		if (arr_cap[AMD_IP_DMA] &&
			is_reset_enable(AMD_IP_DMA, AMDGPU_RESET_TYPE_PER_QUEUE, &pci)) {
			igt_dynamic_f("amdgpu-deadlock-sdma-bad-reg-test")
			bad_access_ring_helper(device, CMD_STREAM_TRANS_BAD_REG_ADDRESS,
					AMDGPU_HW_IP_DMA, &pci, false);
		}
	}

	igt_describe("Test-GPU-reset-by-sdma-bad-length-with-jobs");
	igt_subtest_with_dynamic("amdgpu-deadlock-sdma-bad-length-test") {
		if (arr_cap[AMD_IP_DMA] &&
			is_reset_enable(AMD_IP_DMA, AMDGPU_RESET_TYPE_PER_QUEUE, &pci)) {
			igt_dynamic_f("amdgpu-deadlock-sdma-bad-length-test")
			bad_access_ring_helper(device, CMD_STREAM_EXEC_INVALID_PACKET_LENGTH,
					AMDGPU_HW_IP_DMA, &pci, false);
		}
	}

#ifdef AMDGPU_USERQ_ENABLED
	igt_describe("Test-GPU-reset-by-access-gfx-illegal-reg-umq");
	igt_subtest_with_dynamic("amdgpu-gfx-illegal-reg-access-umq") {
		if (enable_test && userq_arr_cap[AMD_IP_GFX] &&
		    is_reset_enable(AMD_IP_GFX, AMDGPU_RESET_TYPE_PER_QUEUE, &pci)) {
			igt_dynamic_f("amdgpu-illegal-reg-access-umq")
			bad_access_ring_helper(device, CMD_STREAM_TRANS_BAD_REG_ADDRESS,
					       AMDGPU_HW_IP_GFX, &pci, true);
		}
	}

	igt_describe("Test-GPU-reset-by-sdma-badop-with-jobs");
	igt_subtest_with_dynamic("amdgpu-deadlock-sdma-badop-test-umq") {
		if (enable_test && userq_arr_cap[AMD_IP_DMA] &&
			is_reset_enable(AMD_IP_DMA, AMDGPU_RESET_TYPE_PER_QUEUE, &pci)) {
			igt_dynamic_f("amdgpu-deadlock-sdma-badop-test-umq")
			bad_access_ring_helper(device, CMD_STREAM_EXEC_INVALID_OPCODE,
					AMDGPU_HW_IP_DMA, &pci, true);
		}
	}

	igt_describe("Test-GPU-reset-by-sdma-bad-mem-with-jobs");
	igt_subtest_with_dynamic("amdgpu-deadlock-sdma-bad-mem-test-umq") {
		if (enable_test && userq_arr_cap[AMD_IP_DMA] &&
			is_reset_enable(AMD_IP_DMA, AMDGPU_RESET_TYPE_PER_QUEUE, &pci)) {
			igt_dynamic_f("amdgpu-deadlock-sdma-bad-mem-test-umq")
			bad_access_ring_helper(device, CMD_STREAM_TRANS_BAD_MEM_ADDRESS,
					AMDGPU_HW_IP_DMA, &pci, true);
		}
	}

	igt_describe("Test-GPU-reset-by-sdma-bad-reg-with-jobs");
	igt_subtest_with_dynamic("amdgpu-deadlock-sdma-bad-reg-test-umq") {
		if (enable_test && userq_arr_cap[AMD_IP_DMA] &&
			is_reset_enable(AMD_IP_DMA, AMDGPU_RESET_TYPE_PER_QUEUE, &pci)) {
			igt_dynamic_f("amdgpu-deadlock-sdma-bad-reg-test-umq")
			bad_access_ring_helper(device, CMD_STREAM_TRANS_BAD_REG_ADDRESS,
					AMDGPU_HW_IP_DMA, &pci, true);
		}
	}

	igt_describe("Test-GPU-reset-by-sdma-bad-length-with-jobs");
	igt_subtest_with_dynamic("amdgpu-deadlock-sdma-bad-length-test-umq") {
		if (enable_test && userq_arr_cap[AMD_IP_DMA] &&
			is_reset_enable(AMD_IP_DMA, AMDGPU_RESET_TYPE_PER_QUEUE, &pci)) {
			igt_dynamic_f("amdgpu-deadlock-sdma-bad-length-test-umq")
			bad_access_ring_helper(device, CMD_STREAM_EXEC_INVALID_PACKET_LENGTH,
					AMDGPU_HW_IP_DMA, &pci, true);
		}
	}

	igt_describe("Test-GPU-reset-by-flooding-sdma-ring-with-jobs");
	igt_subtest_with_dynamic("amdgpu-deadlock-sdma-umq") {
		if (enable_test && userq_arr_cap[AMD_IP_DMA]) {
			igt_dynamic_f("amdgpu-deadlock-sdma-umq")
			amdgpu_wait_memory_helper(device, AMDGPU_HW_IP_DMA, &pci, true);
		}
	}
#endif

	igt_fixture() {
		amdgpu_device_deinitialize(device);
		drm_close_driver(fd);
		log_total_time(false, igt_test_name());
	}
}
