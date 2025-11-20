// SPDX-License-Identifier: MIT
// Copyright 2025 Advanced Micro Devices, Inc.

#include "lib/amdgpu/amd_vcn_shared.h"
#include "lib/amdgpu/amd_ip_blocks.h"

static bool
is_queue_tests_enable(amdgpu_device_handle device_handle,
	struct mmd_shared_context *context, struct pci_addr *pci)
{
	bool ret = is_vcn_tests_enable(device_handle, context);

	if (!ret)
		return false;

	if (!is_reset_enable(AMD_IP_VCN_UNIFIED, AMDGPU_RESET_TYPE_PER_QUEUE, pci)) {
		igt_info("The ASIC does NOT support vcn queue reset\n");
		return false;
	} else if (context->vcn_ip_version_major < 4) {
		igt_info("The vcn ip does NOT support vcn queue reset\n");
		return false;
	}

	return ret;
}

static int
vcn_queue_test(amdgpu_device_handle device_handle,
	struct mmd_shared_context *shared_context, int err)
{
	struct mmd_context context = {};
	struct vcn_context v_context = {};
	signed int ip;
	int len, ret;

	ret = mmd_context_init(device_handle, &context);
	igt_require(ret == 0);

	context.num_resources  = 0;
	alloc_resource(device_handle, &v_context.session_ctx_buf, 32 * 4096, AMDGPU_GEM_DOMAIN_GTT);
	context.resources[context.num_resources++] = v_context.session_ctx_buf.handle;
	context.resources[context.num_resources++] = context.ib_handle;
	len = 0;
	vcn_dec_cmd(shared_context, &context, &v_context, v_context.session_ctx_buf.addr,
			DECODE_CMD_SESSION_CONTEXT_BUFFER, &len, err);

	amdgpu_cs_sq_ib_tail(&v_context, context.ib_cpu + len);
	ip = AMDGPU_HW_IP_VCN_ENC;
	submit(device_handle, &context, len, ip);

	/*
	 * For job timeout test case, submit job will return error,
	 * here needn't return error.
	 */

	mmd_context_clean(device_handle, &context);
	return 0;
}

igt_main
{
	amdgpu_device_handle device;
	struct mmd_context context = {};
	struct mmd_shared_context shared_context = {};
	int fd = -1;
	uint32_t major, minor;
	struct pci_addr pci;
	int err;

	igt_fixture() {
		fd = drm_open_driver(DRIVER_AMDGPU);
		err = amdgpu_device_initialize(fd, &major, &minor, &device);
		igt_require(err == 0);
		igt_info("Initialized amdgpu, driver version %d.%d\n",
			 major, minor);
		err = mmd_shared_context_init(device, &shared_context);
		igt_require(err == 0);
		err = mmd_context_init(device, &context);
		igt_require(err == 0);
		igt_skip_on(!is_vcn_tests_enable(device, &shared_context));
		igt_skip_on_f(!shared_context.dec_ring && !shared_context.enc_ring, "vcn no decorder and encoder rings\n");
		igt_skip_on(get_pci_addr_from_fd(fd, &pci));
		igt_info("PCI Address: domain %04x, bus %02x, device %02x, function %02x\n",
				pci.domain, pci.bus, pci.device, pci.function);
		igt_skip_on(!is_queue_tests_enable(device, &shared_context, &pci));
		shared_context.ip_type = AMD_IP_VCN_UNIFIED;
	}

	igt_describe("Test whether vcn queue");
	igt_subtest("vcn-decoder-queue-reset-test") {
		err = (int)INVALID_DECODER_IB_SIZE;
		mm_queue_test_helper(device, &shared_context, vcn_queue_test, err, &pci);
	}

	igt_fixture() {
		mmd_context_clean(device, &context);
		amdgpu_device_deinitialize(device);
		drm_close_driver(fd);
	}
}
