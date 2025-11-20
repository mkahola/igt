// SPDX-License-Identifier: MIT
/*
 * Copyright 2014 Advanced Micro Devices, Inc.
 * Copyright 2022 Advanced Micro Devices, Inc.
 * Copyright 2023 Advanced Micro Devices, Inc.
 */
#include <amdgpu.h>
#include <amdgpu_drm.h>
#include "drmtest.h"
#include "igt.h"

static void
amdgpu_stable_pstate_test(amdgpu_device_handle device_handle)
{
	int r;
	amdgpu_context_handle context_handle;
	uint32_t current_pstate, new_pstate;

	r = amdgpu_cs_ctx_create(device_handle, &context_handle);
	igt_assert_eq(r, 0);

	r = amdgpu_cs_ctx_stable_pstate(context_handle,
					AMDGPU_CTX_OP_GET_STABLE_PSTATE,
					0, &current_pstate);
	igt_assert_eq(r, 0);
	igt_assert_eq(current_pstate, AMDGPU_CTX_STABLE_PSTATE_NONE);
	r = amdgpu_cs_ctx_stable_pstate(context_handle,
					AMDGPU_CTX_OP_SET_STABLE_PSTATE,
					AMDGPU_CTX_STABLE_PSTATE_PEAK, NULL);
	igt_assert_eq(r, 0);

	r = amdgpu_cs_ctx_stable_pstate(context_handle,
					AMDGPU_CTX_OP_GET_STABLE_PSTATE,
					0, &new_pstate);
	igt_assert_eq(r, 0);
	igt_assert_eq(new_pstate, AMDGPU_CTX_STABLE_PSTATE_PEAK);

	r = amdgpu_cs_ctx_free(context_handle);
	igt_assert_eq(r, 0);

}

igt_main
{
	amdgpu_device_handle device;
	int fd = -1;

	igt_fixture() {
		uint32_t major, minor;
		int err;

		fd = drm_open_driver(DRIVER_AMDGPU);

		err = amdgpu_device_initialize(fd, &major, &minor, &device);
		igt_require(err == 0);

		igt_info("Initialized amdgpu, driver version %d.%d\n",
			 major, minor);
	}

	igt_subtest("amdgpu_pstate")
	amdgpu_stable_pstate_test(device);

	igt_fixture() {
		amdgpu_device_deinitialize(device);
		drm_close_driver(fd);
	}
}

