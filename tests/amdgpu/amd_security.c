// SPDX-License-Identifier: MIT
/*
 * Copyright 2019 Advanced Micro Devices, Inc.
 * Copyright 2023 Advanced Micro Devices, Inc.
 */

#include <amdgpu.h>

#include "igt.h"

#include "lib/amdgpu/amd_memory.h"
#include "lib/amdgpu/amd_command_submission.h"

/* --------------------- Secure bounce test ------------------------ *
 *
 * The secure bounce test tests that we can evict a TMZ buffer,
 * and page it back in, via a bounce buffer, as it encryption/decryption
 * depends on its physical address, and have the same data, i.e. data
 * integrity is preserved.
 *
 * The steps are as follows (from Christian K.):
 *
 * Buffer A which is TMZ protected and filled by the CPU with a
 * certain pattern. That the GPU is reading only random nonsense from
 * that pattern is irrelevant for the test.
 *
 * This buffer A is then secure copied into buffer B which is also
 * TMZ protected.
 *
 * Buffer B is moved around, from VRAM to GTT, GTT to SYSTEM,
 * etc.
 *
 * Then, we use another secure copy of buffer B back to buffer A.
 *
 * And lastly we check with the CPU the pattern.
 *
 * Assuming that we don't have memory contention and buffer A stayed
 * at the same place, we should still see the same pattern when read
 * by the CPU.
 *
 * If we don't see the same pattern then something in the buffer
 * migration code is not working as expected.
 */

#define PACKET_LCOPY_SIZE         8
#define PACKET_NOP_SIZE          16
#define SECURE_BUFFER_SIZE       (4 * 1024 * sizeof(secure_pattern))

static uint32_t
get_handle(struct amdgpu_bo *bo)
{
	uint32_t handle;
	int r;

	r = amdgpu_bo_export(bo, amdgpu_bo_handle_type_kms, &handle);
	igt_assert_eq(r, 0);

	return handle;
}

static void
amdgpu_sdma_nop(uint32_t *packet, uint32_t nop_count)
{
	/* A packet of the desired number of NOPs.
	 */
	packet[0] = htole32(nop_count << 16);
	for ( ; nop_count > 0; nop_count--)
		packet[nop_count-1] = 0;
}

/**
 * amdgpu_bo_lcopy -- linear copy with TMZ set, using sDMA
 * @dev: AMDGPU device to which both buffer objects belong to
 * @ring_context: aux struct which has destination and source buffer objects
 * @size: size of memory to move, in bytes.
 * @secure: Set to 1 to perform secure copy, 0 for clear
 *
 * Issues and waits for completion of a Linear Copy with TMZ
 * set, to the sDMA engine. @size should be a multiple of
 * at least 16 bytes.
 */
static void
amdgpu_bo_lcopy(amdgpu_device_handle device,
		struct amdgpu_ring_context *ring_context,
		const struct amdgpu_ip_block_version *ip_block, uint32_t size,
		uint32_t secure)
{
	ring_context->pm4 = calloc(PACKET_LCOPY_SIZE, sizeof(*ring_context->pm4));
	ring_context->secure = secure;
	ring_context->pm4_size = PACKET_LCOPY_SIZE;
	ring_context->pm4_dw = PACKET_LCOPY_SIZE;
	ring_context->res_cnt = 2;
	ring_context->write_length =  size;
	igt_assert(ring_context->pm4);
	ip_block->funcs->copy_linear(ip_block->funcs, ring_context,
			&ring_context->pm4_dw);
	amdgpu_test_exec_cs_helper(device, ip_block->type, ring_context, 0);
	free(ring_context->pm4);
}

/**
 * amdgpu_bo_move -- Evoke a move of the buffer object (BO)
 * @dev: device to which this buffer object belongs to
 * @ring_context: aux struct which has destination and source buffer objects
 * @whereto: one of AMDGPU_GEM_DOMAIN_xyz
 * @secure: set to 1 to submit secure IBs
 *
 * Evokes a move of the buffer object @bo to the GEM domain
 * descibed by @whereto.
 *
 * Returns 0 on success; -errno on error.
 */
static void
amdgpu_bo_move(amdgpu_device_handle device, int fd,
		struct amdgpu_ring_context *ring_context,
		const struct amdgpu_ip_block_version *ip_block, uint64_t whereto,
		uint32_t secure)
{
	int r;
	struct drm_amdgpu_gem_op gop = {
		.handle  = get_handle(ring_context->bo2),
		.op      = AMDGPU_GEM_OP_SET_PLACEMENT,
		.value   = whereto,
	};

	ring_context->pm4 = calloc(PACKET_NOP_SIZE, sizeof(*ring_context->pm4));
	ring_context->secure = secure;
	ring_context->pm4_size = PACKET_NOP_SIZE;
	ring_context->pm4_dw = PACKET_NOP_SIZE;
	ring_context->res_cnt = 1;
	igt_assert(ring_context->pm4);

	/* Change the buffer's placement.
	 */
	r = drmIoctl(fd, DRM_IOCTL_AMDGPU_GEM_OP, &gop);
	igt_assert_eq(r, 0);

	/* Now issue a NOP to actually evoke the MM to move
	 * it to the desired location.
	 */
	amdgpu_sdma_nop(ring_context->pm4, PACKET_NOP_SIZE);
	amdgpu_test_exec_cs_helper(device, ip_block->type, ring_context, 0);
	free(ring_context->pm4);
}

/* Safe, O Sec!
 */
static const uint8_t secure_pattern[] = { 0x5A, 0xFE, 0x05, 0xEC };

static void
amdgpu_secure_bounce(amdgpu_device_handle device_handle, int fd,
		struct drm_amdgpu_info_hw_ip  *sdma_info,
		const struct amdgpu_ip_block_version *ip_block, bool secure)
{
	struct amdgpu_ring_context *ring_context;

	long page_size;
	uint8_t *pp;
	int r;
	uint64_t tmp_mc;

	ring_context = calloc(1, sizeof(*ring_context));
	igt_assert(ring_context);

	page_size = sysconf(_SC_PAGESIZE);
	r = amdgpu_cs_ctx_create(device_handle, &ring_context->context_handle);
	igt_assert_eq(r, 0);

	/* Use the first present ring.
	 */
	ring_context->ring_id = ffs(sdma_info->available_rings) - 1;
	if (ring_context->ring_id == -1)
		igt_assert(false);


	/* Allocate a buffer named Alice (bo, bo_cpu, bo_mc) in VRAM. */
	r = amdgpu_bo_alloc_and_map_raw(device_handle, SECURE_BUFFER_SIZE,
						page_size,	AMDGPU_GEM_DOMAIN_VRAM,
						secure == true ? AMDGPU_GEM_CREATE_ENCRYPTED : 0, 0,
						&ring_context->bo,
						(void **)&ring_context->bo_cpu,
						&ring_context->bo_mc,
						&ring_context->va_handle);
	igt_assert_eq(r, 0);

	/* Fill Alice with a pattern */
	for (pp = (__typeof__(pp))ring_context->bo_cpu;
	     pp < (__typeof__(pp)) ring_context->bo_cpu + SECURE_BUFFER_SIZE;
	     pp += sizeof(secure_pattern))
		memcpy(pp, secure_pattern, sizeof(secure_pattern));

	/* Allocate a buffer named Bob(bo2, bo_cpu2, bo_mc2)  in VRAM.
	 */
	r = amdgpu_bo_alloc_and_map_raw(device_handle, SECURE_BUFFER_SIZE,
						page_size, AMDGPU_GEM_DOMAIN_VRAM,
						secure == true ? AMDGPU_GEM_CREATE_ENCRYPTED : 0, 0,
						&ring_context->bo2,
						(void **)&ring_context->bo2_cpu,
						&ring_context->bo_mc2,
						&ring_context->va_handle2);
	igt_assert_eq(r, 0);

	/* sDMA TMZ copy from Alice to Bob */
	ring_context->resources[0] = ring_context->bo2;	// Bob
	ring_context->resources[1] = ring_context->bo;	// Alice

	amdgpu_bo_lcopy(device_handle, ring_context, ip_block, SECURE_BUFFER_SIZE,
			secure == true ? 1 : 0);

	/* Move Bob to the GTT domain. */
	amdgpu_bo_move(device_handle, fd, ring_context, ip_block,
			AMDGPU_GEM_DOMAIN_GTT, 0);

	/* Clean Alice first before do the copy from bob. */
	memset((void *)ring_context->bo_cpu, 0, SECURE_BUFFER_SIZE);

	/* sDMA TMZ copy from Bob to Alice.
	 * bo is a source ,bo2 is destination
	 */

	ring_context->resources[0] = ring_context->bo; // Alice
	ring_context->resources[1] = ring_context->bo2; // Bob

	/*
	 * Swap mc addresses between Bob(bo_mc2) and Alice(bo_mc) since we copy now
	 * from Bob to Alice and using ASIC dependant implementation sdma_ring_copy_linear
	 * which uses context->bo_mc as source and context->bo_mc2 as destination
	 */
	tmp_mc = ring_context->bo_mc2;
	ring_context->bo_mc2 = ring_context->bo_mc;
	ring_context->bo_mc = tmp_mc;

	/* sDMA TMZ copy from Bob to Alice. */
	amdgpu_bo_lcopy(device_handle, ring_context, ip_block, SECURE_BUFFER_SIZE,
			secure == true ? 1 : 0);

	/* Verify the content of Alice if it matches to pattern */
	for (pp = (__typeof__(pp))ring_context->bo_cpu;
	     pp < (__typeof__(pp)) ring_context->bo_cpu + SECURE_BUFFER_SIZE;
	     pp += sizeof(secure_pattern)) {
		r = memcmp(pp, secure_pattern, sizeof(secure_pattern));
		if (r) {
			// test failure
			igt_assert(false);
			break;
		}
	}
	amdgpu_bo_unmap_and_free(ring_context->bo, ring_context->va_handle,
			ring_context->bo_mc, SECURE_BUFFER_SIZE);
	amdgpu_bo_unmap_and_free(ring_context->bo2, ring_context->va_handle2,
			ring_context->bo_mc2, SECURE_BUFFER_SIZE);
	amdgpu_cs_ctx_free(ring_context->context_handle);
	free(ring_context);
}


static void
amdgpu_security_alloc_buf_test(amdgpu_device_handle device_handle)
{
	amdgpu_bo_handle bo;
	amdgpu_va_handle va_handle;
	uint64_t bo_mc;

	/* Test secure buffer allocation in VRAM */
	bo = gpu_mem_alloc(device_handle, 4096, 4096,
			   AMDGPU_GEM_DOMAIN_VRAM,
			   AMDGPU_GEM_CREATE_ENCRYPTED,
			   &bo_mc, &va_handle);

	gpu_mem_free(bo, va_handle, bo_mc, 4096);

	/* Test secure buffer allocation in system memory */
	bo = gpu_mem_alloc(device_handle, 4096, 4096,
			   AMDGPU_GEM_DOMAIN_GTT,
			   AMDGPU_GEM_CREATE_ENCRYPTED,
			   &bo_mc, &va_handle);

	gpu_mem_free(bo, va_handle, bo_mc, 4096);

	/* Test secure buffer allocation in invisible VRAM */
	bo = gpu_mem_alloc(device_handle, 4096, 4096,
			   AMDGPU_GEM_DOMAIN_GTT,
			   AMDGPU_GEM_CREATE_ENCRYPTED |
			   AMDGPU_GEM_CREATE_NO_CPU_ACCESS,
			   &bo_mc, &va_handle);

	gpu_mem_free(bo, va_handle, bo_mc, 4096);
}

static bool
is_security_tests_enable(amdgpu_device_handle device_handle,
		const struct amdgpu_gpu_info *gpu_info, uint32_t major, uint32_t minor)
{
	bool enable = true;

	if (!(gpu_info->ids_flags & AMDGPU_IDS_FLAGS_TMZ)) {
		igt_info("Don't support TMZ (trust memory zone), security test is disabled\n");
		enable = false;
	}

	if ((major < 3) ||
		((major == 3) && (minor < 37))) {
		igt_info("Don't support TMZ (trust memory zone), kernel DRM version (%d.%d)\n",
				major, minor);
		enable = false;
	}

	return enable;
}

int igt_main()
{
	amdgpu_device_handle device;
	struct amdgpu_gpu_info gpu_info = {};
	struct drm_amdgpu_info_hw_ip  sdma_info = {};
	int r, fd = -1;
	bool is_secure = true;
	bool userq_arr_cap[AMD_IP_MAX] = {0};

#ifdef AMDGPU_USERQ_ENABLED
	bool enable_test;
	const char *env = getenv("AMDGPU_ENABLE_USERQTEST");

	enable_test = env && atoi(env);
#endif

	igt_fixture() {
		uint32_t major, minor;
		int err;

		fd = drm_open_driver(DRIVER_AMDGPU);
		err = amdgpu_device_initialize(fd, &major, &minor, &device);
		igt_require(err == 0);
		igt_info("Initialized amdgpu, driver version %d.%d\n",
			 major, minor);
		err = amdgpu_query_gpu_info(device, &gpu_info);
		igt_assert_eq(err, 0);
		r = setup_amdgpu_ip_blocks(major, minor,  &gpu_info, device);
		igt_assert_eq(r, 0);
		r = amdgpu_query_hw_ip_info(device, AMDGPU_HW_IP_DMA, 0, &sdma_info);
		igt_assert_eq(r, 0);
		asic_userq_readiness(device, userq_arr_cap);
		igt_skip_on(!is_security_tests_enable(device, &gpu_info, major, minor));
	}

	igt_describe("amdgpu security alloc buf test");
	igt_subtest("amdgpu-security-alloc-buf-test")
	amdgpu_security_alloc_buf_test(device);

	igt_describe("amdgpu sdma command submission write linear helper");
	igt_subtest("sdma-write-linear-helper-secure")
	amdgpu_command_submission_write_linear_helper(device,
			get_ip_block(device, AMDGPU_HW_IP_DMA), is_secure, false);

	igt_describe("amdgpu gfx command submission write linear helper");
	igt_subtest("gfx-write-linear-helper-secure")
	 amdgpu_command_submission_write_linear_helper(device,
			get_ip_block(device, AMDGPU_HW_IP_GFX), is_secure, false);

	/* dynamic test based on sdma_info.available rings */
	igt_describe("amdgpu secure bounce");
	igt_subtest("amdgpu-secure-bounce")
	amdgpu_secure_bounce(device, fd, &sdma_info, get_ip_block(device,
			AMDGPU_HW_IP_DMA), is_secure);

#ifdef AMDGPU_USERQ_ENABLED
	igt_describe("amdgpu gfx command submission write linear helper with user queue");
	igt_subtest("gfx-write-linear-helper-secure-umq")
	if (enable_test && userq_arr_cap[AMD_IP_GFX])
		amdgpu_command_submission_write_linear_helper(device,
						get_ip_block(device, AMDGPU_HW_IP_GFX),
						is_secure, true);

	igt_describe("amdgpu compute command submission write linear helper with user queue");
	igt_subtest("compute-write-linear-helper-secure-umq")
	if (enable_test && userq_arr_cap[AMD_IP_COMPUTE])
		amdgpu_command_submission_write_linear_helper(device,
							get_ip_block(device, AMDGPU_HW_IP_COMPUTE),
							is_secure, true);
#endif

	igt_fixture() {
		amdgpu_device_deinitialize(device);
		drm_close_driver(fd);
	}
}


