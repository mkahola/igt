// SPDX-License-Identifier: MIT
/*
 * Copyright 2023 Advanced Micro Devices, Inc.
 */

#include <stdio.h>
#include <amdgpu.h>
#include <amdgpu_drm.h>

#include "igt.h"
#include "lib/amdgpu/amd_memory.h"


#define BUFFER_SIZE (4*1024)
#define BUFFER_ALIGN (4*1024)

struct bo_data {
	amdgpu_bo_handle buffer_handle;
	uint64_t virtual_mc_base_address;
	amdgpu_va_handle va_handle;
};

static int
amdgpu_bo_init(amdgpu_device_handle device_handle, struct bo_data *bo)
{
	struct amdgpu_bo_alloc_request req = {0};
	int r;

	req.alloc_size = BUFFER_SIZE;
	req.phys_alignment = BUFFER_ALIGN;
	req.preferred_heap = AMDGPU_GEM_DOMAIN_GTT;

	r = amdgpu_bo_alloc(device_handle, &req, &bo->buffer_handle);
	if (r)
		return r;

	r = amdgpu_va_range_alloc(device_handle,
				  amdgpu_gpu_va_range_general,
				  BUFFER_SIZE, BUFFER_ALIGN, 0,
				  &bo->virtual_mc_base_address, &bo->va_handle, 0);
	if (r)
		goto error_va_alloc;

	r = amdgpu_bo_va_op(bo->buffer_handle, 0, BUFFER_SIZE,
			bo->virtual_mc_base_address, 0, AMDGPU_VA_OP_MAP);
	if (r)
		goto error_va_map;

	return r;

error_va_map:
	amdgpu_va_range_free(bo->va_handle);

error_va_alloc:
	amdgpu_bo_free(bo->buffer_handle);
	return r;
}

static void
amdgpu_bo_clean(amdgpu_device_handle device_handle, struct bo_data *bo)
{
	int r;

	r = amdgpu_bo_va_op(bo->buffer_handle, 0, BUFFER_SIZE,
			    bo->virtual_mc_base_address, 0,
			    AMDGPU_VA_OP_UNMAP);
	igt_assert_eq(r, 0);

	r = amdgpu_va_range_free(bo->va_handle);
	igt_assert_eq(r, 0);
	r = amdgpu_bo_free(bo->buffer_handle);
	igt_assert_eq(r, 0);
}

static void
amdgpu_bo_export_import_do_type(amdgpu_device_handle device_handle,
		struct bo_data *bo, enum amdgpu_bo_handle_type type)
{
	struct amdgpu_bo_import_result res = {0};
	uint32_t shared_handle;
	int r;

	r = amdgpu_bo_export(bo->buffer_handle, type, &shared_handle);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_import(device_handle, type, shared_handle, &res);
	igt_assert_eq(r, 0);

	igt_assert(res.buf_handle == bo->buffer_handle);
	igt_assert_eq(res.alloc_size, BUFFER_SIZE);

	r = amdgpu_bo_free(res.buf_handle);
	igt_assert_eq(r, 0);
}

static void
amdgpu_bo_export_import(amdgpu_device_handle device, struct bo_data *bo)
{
	amdgpu_bo_export_import_do_type(device, bo,
			amdgpu_bo_handle_type_gem_flink_name);
	amdgpu_bo_export_import_do_type(device, bo,
			amdgpu_bo_handle_type_dma_buf_fd);
}

static void
amdgpu_bo_metadata(amdgpu_device_handle device, struct bo_data *bo)
{
	struct amdgpu_bo_metadata meta = {0};
	struct amdgpu_bo_info info = {0};
	int r;

	meta.size_metadata = 4;
	meta.umd_metadata[0] = 0xdeadbeef;

	r = amdgpu_bo_set_metadata(bo->buffer_handle, &meta);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_query_info(bo->buffer_handle, &info);
	igt_assert_eq(r, 0);

	igt_assert_eq(info.metadata.size_metadata, 4);
	igt_assert_eq(info.metadata.umd_metadata[0], 0xdeadbeef);
}

static void
amdgpu_bo_map_unmap(amdgpu_device_handle device, struct bo_data *bo)
{
	uint32_t *ptr;
	int i, r;

	r = amdgpu_bo_cpu_map(bo->buffer_handle, (void **)&ptr);
	igt_assert_eq(r, 0);

	for (i = 0; i < (BUFFER_SIZE / 4); ++i)
		ptr[i] = 0xdeadbeef;

	r = amdgpu_bo_cpu_unmap(bo->buffer_handle);
	igt_assert_eq(r, 0);
}

static void
amdgpu_memory_alloc(amdgpu_device_handle device_handle)
{
	amdgpu_bo_handle bo;
	amdgpu_va_handle va_handle;
	uint64_t bo_mc;

	/* Test visible VRAM */
	bo = gpu_mem_alloc(device_handle,
			4096, 4096,
			AMDGPU_GEM_DOMAIN_VRAM,
			AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED,
			&bo_mc, &va_handle);

	gpu_mem_free(bo, va_handle, bo_mc, 4096);

	/* Test invisible VRAM */
	bo = gpu_mem_alloc(device_handle,
			4096, 4096,
			AMDGPU_GEM_DOMAIN_VRAM,
			AMDGPU_GEM_CREATE_NO_CPU_ACCESS,
			&bo_mc, &va_handle);

	gpu_mem_free(bo, va_handle, bo_mc, 4096);

	/* Test GART cacheable */
	bo = gpu_mem_alloc(device_handle,
			4096, 4096,
			AMDGPU_GEM_DOMAIN_GTT,
			0, &bo_mc, &va_handle);

	gpu_mem_free(bo, va_handle, bo_mc, 4096);

	/* Test GART USWC */
	bo = gpu_mem_alloc(device_handle,
			4096, 4096,
			AMDGPU_GEM_DOMAIN_GTT,
			AMDGPU_GEM_CREATE_CPU_GTT_USWC,
			&bo_mc, &va_handle);

	gpu_mem_free(bo, va_handle, bo_mc, 4096);

	/* Test GDS */
	bo = gpu_mem_alloc(device_handle, 1024, 0,
			AMDGPU_GEM_DOMAIN_GDS, 0,
			&bo_mc, &va_handle);

	gpu_mem_free(bo, va_handle, bo_mc, 4096);
	/* Test GWS */
	bo = gpu_mem_alloc(device_handle, 1, 0,
			AMDGPU_GEM_DOMAIN_GWS, 0,
			&bo_mc, &va_handle);
	gpu_mem_free(bo, va_handle, bo_mc, 4096);
	/* Test OA */
	bo = gpu_mem_alloc(device_handle, 1, 0,
			AMDGPU_GEM_DOMAIN_OA, 0,
			&bo_mc, &va_handle);
	gpu_mem_free(bo, va_handle, bo_mc, 4096);
}

static void
amdgpu_mem_fail_alloc(amdgpu_device_handle device_handle)
{
	int r;
	struct amdgpu_bo_alloc_request req = {0};
	amdgpu_bo_handle buf_handle;

	/* Test impossible mem allocation, 1TB */
	req.alloc_size = 0xE8D4A51000;
	req.phys_alignment = 4096;
	req.preferred_heap = AMDGPU_GEM_DOMAIN_VRAM;
	req.flags = AMDGPU_GEM_CREATE_NO_CPU_ACCESS;

	r = amdgpu_bo_alloc(device_handle, &req, &buf_handle);
	igt_assert_eq(r, -ENOMEM);

	if (!r) {
		r = amdgpu_bo_free(buf_handle);
		igt_assert_eq(r, 0);
	}
}

static void
amdgpu_bo_find_by_cpu_mapping(amdgpu_device_handle device_handle)
{
	amdgpu_bo_handle bo_handle, find_bo_handle;
	amdgpu_va_handle va_handle;
	void *bo_cpu;
	uint64_t bo_mc_address;
	uint64_t offset;
	int r;

	r = amdgpu_bo_alloc_and_map(device_handle, 4096, 4096,
				    AMDGPU_GEM_DOMAIN_GTT, 0,
				    &bo_handle, &bo_cpu,
				    &bo_mc_address, &va_handle);
	igt_assert_eq(r, 0);

	r = amdgpu_find_bo_by_cpu_mapping(device_handle,
					  bo_cpu,
					  4096,
					  &find_bo_handle,
					  &offset);
	igt_assert_eq(r, 0);
	igt_assert_eq(offset, 0);

	amdgpu_bo_unmap_and_free(bo_handle, va_handle,
				     bo_mc_address, 4096);
}

int igt_main()
{
	amdgpu_device_handle device;
	struct bo_data bo;
	int fd = -1;

	igt_fixture() {
		uint32_t major, minor;
		int err;

		fd = drm_open_driver(DRIVER_AMDGPU);
		err = amdgpu_device_initialize(fd, &major, &minor, &device);
		igt_require(err == 0);
		igt_info("Initialized amdgpu, driver version %d.%d\n",
			 major, minor);
		err = amdgpu_bo_init(device, &bo);
		igt_require(err == 0);
	}

	igt_subtest("amdgpu_bo_export_import")
	amdgpu_bo_export_import(device, &bo);

	igt_subtest("amdgpu_bo_metadata")
	amdgpu_bo_metadata(device, &bo);

	igt_subtest("amdgpu_bo_map_unmap")
	amdgpu_bo_map_unmap(device, &bo);

	igt_subtest("amdgpu_memory_alloc")
	amdgpu_memory_alloc(device);

	igt_subtest("amdgpu_mem_fail_alloc")
	amdgpu_mem_fail_alloc(device);

	igt_subtest("amdgpu_bo_find_by_cpu_mapping")
	amdgpu_bo_find_by_cpu_mapping(device);

	igt_fixture() {
		amdgpu_bo_clean(device, &bo);
		amdgpu_device_deinitialize(device);
		close(fd);
	}
}

