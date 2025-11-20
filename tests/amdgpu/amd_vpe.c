// SPDX-License-Identifier: MIT
/*
 * Copyright 2023 Advanced Micro Devices, Inc.
 */

#include <amdgpu.h>
#include "amdgpu_drm.h"

#include "igt.h"

#include "lib/amdgpu/amd_mmd_shared.h"

IGT_TEST_DESCRIPTION("Test VPE functionality");

#define MAX_RESOURCES		16

#define PLANE_WIDTH		1024
#define PLANE_HEIGHT		256
#define PLANE_SIZE		(PLANE_WIDTH*PLANE_HEIGHT*4)

#define SRC_PLANE_PATTERN	0x12345678
#define DST_PLANE_PATTERN	0xff123456

static uint32_t vpe_descriptor[] = {
0x00000001, 0x33002200, 0xff000021, 0x00000003, 0x33002234, 0xff000021, 0x33002328, 0xff000021,
0x33002384, 0xff000021, 0x330023c0, 0xff000021,
};

static uint32_t vpe_config[] = {
0x00000002, 0x00000000, 0xbeefbe00, 0xff005678, 0x000003ff, 0x00000000, 0x00ff43ff, 0x00000000,
0xbeefbe00, 0xff005679, 0x000003ff, 0x00000000, 0x00ff43ff, 0x003b0003, 0x00047808, 0x00000809,
0x0004780c, 0x000000e4, 0x00047d10, 0x00000009, 0x00047d14, 0x00000101, 0x00047d18, 0x00000000,
0x00047d1c, 0x00000000, 0x00047d20, 0x00000000, 0x00047d24, 0x0001f010, 0x00047d28, 0x0001f010,
0x00047d2c, 0x0001f010, 0x00547ee9, 0x00002000, 0x00000000, 0x20000000, 0x00000000, 0x00000000,
0x00002000, 0x00047ee4, 0x00000001, 0x00047ee0, 0x00000000, 0x00047f24, 0x00000000, 0x00047fc4,
0x00000000, 0x00547f05, 0x00002000, 0x00000000, 0x20000000, 0x00000000, 0x00000000, 0x00002000,
0x00047f00, 0x00000001, 0x00049700, 0x00000000, 0x00049704, 0x0000000f, 0x00049f30, 0x00000000,
0x00049708, 0x00000000, 0x0004970c, 0xffff0462, 0x0004a208, 0x00000000, 0x0004971c, 0x00000000,
0x00047fc0, 0x0001f000, 0x00150003, 0x00047df8, 0x00000001, 0x00047dfc, 0x00000001, 0x00047da8,
0x00000006, 0x00047e18, 0x00000000, 0x0004970c, 0xffff0422, 0x00049710, 0x0001f000, 0x00049714,
0x0001f000, 0x00049718, 0x0001f000, 0x00049720, 0x00000000, 0x00049724, 0x00000000, 0x00049728,
0x00000000, 0x000d0003, 0x00047810, 0x00000000, 0x00047814, 0x01000400, 0x00047818, 0x00000000,
0x0004781c, 0x01000400, 0x00047e00, 0x00000000, 0x00047e04, 0x01000400, 0x00047e08, 0x01000400,
0x00280003, 0x00047820, 0x00000036, 0x00047824, 0x0960f015, 0x0004972c, 0x00000014, 0x0004972c,
0x00000014, 0x0004972c, 0x00000014, 0x00049f90, 0x00000000, 0x00049f94, 0x00000001, 0x00549f99,
0x00002000, 0x00000000, 0x20000000, 0x00000000, 0x00000000, 0x00002000, 0x00049850, 0x00000000,
0x00049f34, 0x00000000, 0x00049f38, 0x02fff000, 0x00049f3c, 0x00fff000, 0x00049f40, 0x00fff000,
0x0004aba0, 0xffff0000, 0x0004aba0, 0xffff0000, 0x0004aacc, 0x00000000, 0x0004aad4, 0x00000013,
0x0004aad4, 0x00000013,
};

static bool is_vpe_tests_enabled(amdgpu_device_handle device_handle,
		struct mmd_shared_context *shared_context)
{
	struct drm_amdgpu_info_hw_ip info;
	int r;

	r = amdgpu_query_hw_ip_info(device_handle, AMDGPU_HW_IP_VPE, 0, &info);
	igt_assert_eq(r, 0);

	shared_context->vpe_ip_version_major = info.hw_ip_version_major;
	shared_context->vpe_ip_version_minor = info.hw_ip_version_minor;
	shared_context->vpe_ring = !!info.available_rings;

	if (!shared_context->vpe_ring) {
		igt_info("VPE no available rings");
		igt_info("VPE fence test disable");
		igt_info("VPE blit test disable");

		return false;
	}

	return true;
}

static void amdgpu_cs_vpe_fence(amdgpu_device_handle device_handle,
				struct mmd_context *context)
{
	const uint32_t test_pattern = 0xdeadbeef;
	uint32_t *ib_cpu = context->ib_cpu;
	struct amdgpu_mmd_bo test_bo;
	int r;

	context->num_resources = 0;
	alloc_resource(device_handle, &test_bo, 4096, AMDGPU_GEM_DOMAIN_GTT);
	context->resources[context->num_resources++] = test_bo.handle;

	r = amdgpu_bo_cpu_map(test_bo.handle, (void **)&test_bo.ptr);
	igt_assert_eq(r, 0);

	memset(test_bo.ptr, 0, 4096);

	memset(ib_cpu, 0, IB_SIZE);

	ib_cpu[0] = 0x5;
	ib_cpu[1] = lower_32_bits(test_bo.addr);
	ib_cpu[2] = upper_32_bits(test_bo.addr);
	ib_cpu[3] = test_pattern;
	ib_cpu[4] = 0x0;
	ib_cpu[5] = 0x0;
	ib_cpu[6] = 0x0;
	ib_cpu[7] = 0x0;

	context->resources[context->num_resources++] = context->ib_handle;

	r = submit(device_handle, context, 8, AMDGPU_HW_IP_VPE);
	igt_assert_eq(r, 0);

	igt_assert_eq(((uint32_t *)test_bo.ptr)[0], test_pattern);

	r = amdgpu_bo_cpu_unmap(test_bo.handle);
	igt_assert_eq(r, 0);

	free_resource(&test_bo);
}

// a in byte 0, b in byte 1, g in byte 2, r in byte 3
static void create_rgba8888(void *addr, uint32_t width, uint32_t height)
{
	uint32_t *ptr = (uint32_t *)addr;

	for (int i = 0; i < height; i++) {
		for (int j = 0; j < width; j++)
			ptr[j] = SRC_PLANE_PATTERN;
		ptr += width;
	}
}
// b in byte 0, g in byte 1, r in byte 2, a in byte 3
static int check_argb8888(void *addr, uint32_t width, uint32_t height)
{
	uint32_t *ptr = (uint32_t *)addr;

	for (int i = 0; i < height; i++) {
		for (int j = 0; j < width; j++)
			if (ptr[j] != DST_PLANE_PATTERN)
				return 1;
		ptr += width;
	}

	return 0;
}

static void amdgpu_cs_vpe_blit(amdgpu_device_handle device_handle,
				struct mmd_context *context)
{
	const uint32_t vpep_config_offsets[] = {0x34, 0x128, 0x184, 0x1c0};
	struct amdgpu_mmd_bo vpe_config_bo, src_plane_bo, dst_plane_bo;
	int r;

	context->num_resources = 0;

	alloc_resource(device_handle, &vpe_config_bo, sizeof(vpe_config), AMDGPU_GEM_DOMAIN_GTT);
	alloc_resource(device_handle, &src_plane_bo, PLANE_SIZE, AMDGPU_GEM_DOMAIN_GTT);
	alloc_resource(device_handle, &dst_plane_bo, PLANE_SIZE, AMDGPU_GEM_DOMAIN_GTT);

	r = amdgpu_bo_cpu_map(vpe_config_bo.handle, (void **)&vpe_config_bo.ptr);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_cpu_map(src_plane_bo.handle, (void **)&src_plane_bo.ptr);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_cpu_map(dst_plane_bo.handle, (void **)&dst_plane_bo.ptr);
	igt_assert_eq(r, 0);

	context->resources[context->num_resources++] = vpe_config_bo.handle;
	context->resources[context->num_resources++] = src_plane_bo.handle;
	context->resources[context->num_resources++] = dst_plane_bo.handle;

	// plane config gpu addr
	*(uint64_t *)(vpe_descriptor + 1) = vpe_config_bo.addr;
	// vpep config0 gpu addr
	*(uint64_t *)(vpe_descriptor + 4) = vpe_config_bo.addr + vpep_config_offsets[0];
	// vpep config1 gpu addr
	*(uint64_t *)(vpe_descriptor + 6) = vpe_config_bo.addr + vpep_config_offsets[1];
	// vpep config2 gpu addr
	*(uint64_t *)(vpe_descriptor + 8) = vpe_config_bo.addr + vpep_config_offsets[2];
	// vpep config3 gpu addr
	*(uint64_t *)(vpe_descriptor + 10) = vpe_config_bo.addr + vpep_config_offsets[3];

	memset(src_plane_bo.ptr, 0, PLANE_SIZE);
	memset(dst_plane_bo.ptr, 0, PLANE_SIZE);
	create_rgba8888(src_plane_bo.ptr, PLANE_WIDTH, PLANE_HEIGHT);

	/* gpu address of src */
	*(uint64_t *)(vpe_config + 2) = src_plane_bo.addr;
	/* gpu address of dst */
	*(uint64_t *)(vpe_config + 8) = dst_plane_bo.addr;

	memset(vpe_config_bo.ptr, 0, sizeof(vpe_config));
	memcpy(vpe_config_bo.ptr, vpe_config, sizeof(vpe_config));

	memset(context->ib_cpu, 0, IB_SIZE);
	memcpy(context->ib_cpu, vpe_descriptor, sizeof(vpe_descriptor));

	context->resources[context->num_resources++] = context->ib_handle;

	r = submit(device_handle, context, sizeof(vpe_descriptor)/4, AMDGPU_HW_IP_VPE);
	igt_assert_eq(r, 0);

	r = check_argb8888(dst_plane_bo.ptr, PLANE_WIDTH, PLANE_HEIGHT);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_cpu_unmap(vpe_config_bo.handle);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_cpu_unmap(src_plane_bo.handle);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_cpu_unmap(dst_plane_bo.handle);
	igt_assert_eq(r, 0);

	free_resource(&vpe_config_bo);
	free_resource(&src_plane_bo);
	free_resource(&dst_plane_bo);
}

int igt_main()
{
	struct mmd_context context = {};
	struct mmd_shared_context shared_context = {};
	amdgpu_device_handle device;
	int fd = -1;

	igt_fixture() {
		uint32_t major, minor;
		int r;

		fd = drm_open_driver(DRIVER_AMDGPU);
		igt_require(fd > 0);

		r = amdgpu_device_initialize(fd, &major, &minor, &device);
		igt_require(r == 0);

		igt_info("Initialized amdgpu, driver version %d.%d\n", major, minor);

		r = mmd_shared_context_init(device, &shared_context);
		igt_require(r == 0);
		r = mmd_context_init(device, &context);
		igt_require(r == 0);

		igt_skip_on(!is_vpe_tests_enabled(device, &shared_context));
	}

	igt_describe("Test VPE fence");
	igt_subtest("vpe-fence-test")
	amdgpu_cs_vpe_fence(device, &context);

	igt_describe("Test VPE blit");
	igt_subtest("vpe-blit-test")
	amdgpu_cs_vpe_blit(device, &context);

	igt_fixture() {
		amdgpu_device_deinitialize(device);
		drm_close_driver(fd);
	}

}
