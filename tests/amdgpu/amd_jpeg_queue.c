// SPDX-License-Identifier: MIT
// Copyright 2025 Advanced Micro Devices, Inc.

#include "lib/amdgpu/amd_jpeg_shared.h"
#include "lib/amdgpu/amd_ip_blocks.h"

static bool
is_queue_tests_enable(amdgpu_device_handle device_handle,
		struct mmd_shared_context *context, struct pci_addr *pci)
{
	bool ret = is_jpeg_tests_enable(device_handle, context);

	if (!ret)
		return false;

	if (!is_reset_enable(AMD_IP_VCN_JPEG, AMDGPU_RESET_TYPE_PER_QUEUE, pci)) {
		igt_info("The ASIC does NOT support jpeg queue reset\n");
		return false;
	} else if (context->vcn_ip_version_major < 4) {
		igt_info("The vcn ip does NOT support jpeg queue reset\n");
		return false;
	}

	return ret;
}

static int
jpeg_queue_decode(amdgpu_device_handle device_handle,
		struct mmd_shared_context *shared_context, int err)
{
	struct amdgpu_mmd_bo dec_buf;
	int size, ret;
	uint32_t idx;
	struct mmd_context acontext = {0};
	struct mmd_context *context = &acontext;
	int64_t dec_buf_addr;

	ret = mmd_context_init(device_handle, context);
	if (ret) {
		igt_info("mmd_context_init fail!\n");
		return ret;
	}

	size = 32 * 1024; /* 8K bitstream + 24K output */
	context->num_resources = 0;
	alloc_resource(device_handle, &dec_buf, size, AMDGPU_GEM_DOMAIN_VRAM);
	context->resources[context->num_resources++] = dec_buf.handle;
	context->resources[context->num_resources++] = context->ib_handle;
	ret = amdgpu_bo_cpu_map(dec_buf.handle, (void **)&dec_buf.ptr);
	if (ret) {
		igt_info("amdgpu_bo_cpu_map map dec_buf fail!\n");
		goto err_handle;
	}
	memcpy(dec_buf.ptr, jpeg_bitstream, sizeof(jpeg_bitstream));

	idx = 0;
	dec_buf_addr = dec_buf.addr;
	if (err == INVALID_DECODER_BITSTREAM_BUFFER)
		dec_buf_addr = 0xdead;

	send_cmd_bitstream_direct(context, dec_buf_addr, &idx);
	send_cmd_target_direct(context, dec_buf_addr + (size / 4), &idx);

	amdgpu_bo_cpu_unmap(dec_buf.handle);
	submit(device_handle, context, idx, AMDGPU_HW_IP_VCN_JPEG);

	/*
	 * For job timeout test case, submit job will return error,
	 * here needn't return error.
	 */
	ret = 0;

err_handle:
	free_resource(&dec_buf);
	mmd_context_clean(device_handle, context);
	return ret;
}

igt_main
{
	amdgpu_device_handle device;
	struct mmd_shared_context shared_context = {};
	int fd = -1;
	uint32_t major, minor;
	int err;
	struct pci_addr pci;

	igt_fixture() {
		fd = drm_open_driver(DRIVER_AMDGPU);
		err = amdgpu_device_initialize(fd, &major, &minor, &device);
		igt_require(err == 0);
		igt_info("Initialized amdgpu, driver version %d.%d\n",
			 major, minor);
		err = mmd_shared_context_init(device, &shared_context);
		igt_require(err == 0);

		igt_skip_on(get_pci_addr_from_fd(fd, &pci));
		igt_info("PCI Address: domain %04x, bus %02x, device %02x, function %02x\n",
				pci.domain, pci.bus, pci.device, pci.function);
		igt_skip_on(!is_queue_tests_enable(device, &shared_context, &pci));
		shared_context.ip_type = AMD_IP_VCN_JPEG;
	}

	igt_describe("Test whether jpeg queue");
	igt_subtest("jpeg-decoder-queue-reset") {
		err = (int)INVALID_DECODER_BITSTREAM_BUFFER;
		mm_queue_test_helper(device, &shared_context, jpeg_queue_decode, err, &pci);
	}

	igt_fixture() {
		amdgpu_device_deinitialize(device);
		drm_close_driver(fd);
	}

}
