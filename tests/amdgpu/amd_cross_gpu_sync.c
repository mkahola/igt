// SPDX-License-Identifier: MIT
// Copyright 2026 Advanced Micro Devices, Inc.
/*
 * amd_cross_gpu_sync - Cross-GPU DMA-buf frame transfer test
 *
 * Simulates a render-on-A / display-on-B pipeline at a configurable
 * refresh rate (default 60 Hz) with CRC-32 frame verification.
 *
 * Test flow per frame:
 *   1. GPU A: SDMA CONSTANT_FILL writes a per-frame pattern into the
 *      shared VRAM buffer (exported as DMA-buf).
 *   2. Configurable refresh-rate delay (simulates vsync interval).
 *   3. GPU B: SDMA COPY_LINEAR reads the imported DMA-buf into a
 *      local staging BO (using ip_block->funcs->copy_linear()).
 *   4. CPU computes CRC-32 of staging data and compares it with the
 *      CRC-32 of the expected per-frame fill pattern.
 *
 * Environment variables:
 *   IGT_CROSS_GPU_REFRESH_HZ   Refresh rate in Hz (default: 60)
 *   IGT_CROSS_GPU_NUM_FRAMES   Number of frames (default: 30)
 *
 * SDMA packets are built with the project IP-block helpers from
 * lib/amdgpu/amd_ip_blocks.c (copy_linear, const_fill macros)
 * and the SDMA opcodes from lib/amdgpu/amd_sdma.h.
 *
 * Subtests:
 *   - "implicit-sync": GPU B submits without explicit fence wait
 *     for GPU A. Relies on DMA-buf implicit fence propagation.
 *   - "explicit-sync": CPU waits for GPU A before GPU B submits.
 *     Control test - should always pass.
 */

#include "igt.h"
#include "drmtest.h"
#include <amdgpu.h>
#include <amdgpu_drm.h>

#include "lib/amdgpu/amd_PM4.h"
#include "lib/amdgpu/amd_sdma.h"
#include "lib/amdgpu/amd_ip_blocks.h"
#include "lib/amdgpu/amd_memory.h"
#include "lib/amdgpu/amd_command_submission.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define BUF_SIZE        (4 * 1024 * 1024)  /* 4 MB shared buffer */
#define PATTERN_SEED    0xDEADBEEFu
#define DEFAULT_HZ      60
#define DEFAULT_FRAMES  30
#define PM4_DW_MAX      256

/* Per-GPU context */
struct gpu_ctx {
	int fd;
	amdgpu_device_handle dev;
	amdgpu_context_handle ctx;
	uint32_t major;
	uint32_t minor;
	struct amdgpu_gpu_info gpu_info;
};

/* Shared DMA-buf state between two GPUs */
struct shared_dmabuf {
	amdgpu_bo_handle shared_bo;
	amdgpu_va_handle shared_va_h;
	uint64_t shared_va;

	int dmabuf_fd;

	amdgpu_bo_handle imported_bo;
	amdgpu_va_handle imported_va_h;
	uint64_t imported_va;
};

/*
 * Software CRC-32 (ISO 3309 / ITU-T V.42 polynomial).
 * No external library dependency.
 */
static void crc32_build_table(uint32_t *tbl)
{
	uint32_t i, j, c;

	for (i = 0; i < 256; i++) {
		c = i;
		for (j = 0; j < 8; j++)
			c = (c >> 1) ^ ((c & 1) ? 0xEDB88320u : 0);
		tbl[i] = c;
	}
}

static uint32_t crc32_buf(const uint32_t *tbl, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	uint32_t crc = 0xFFFFFFFFu;
	size_t i;

	for (i = 0; i < len; i++)
		crc = (crc >> 8) ^ tbl[(crc ^ p[i]) & 0xFF];
	return crc ^ 0xFFFFFFFFu;
}

/*
 * Compute expected CRC-32 for a buffer entirely filled with `value`.
 */
static uint32_t expected_crc32(const uint32_t *tbl, uint32_t value,
			      size_t byte_count)
{
	uint32_t crc = 0xFFFFFFFFu;
	uint8_t tile[4];
	size_t i;

	tile[0] = (uint8_t)(value);
	tile[1] = (uint8_t)(value >> 8);
	tile[2] = (uint8_t)(value >> 16);
	tile[3] = (uint8_t)(value >> 24);

	for (i = 0; i < byte_count; i++)
		crc = (crc >> 8) ^ tbl[(crc ^ tile[i & 3]) & 0xFF];
	return crc ^ 0xFFFFFFFFu;
}

static void gpu_open(struct gpu_ctx *g, int fd)
{
	uint32_t major, minor;
	int r;

	g->fd = fd;
	r = amdgpu_device_initialize(fd, &major, &minor, &g->dev);
	igt_assert_eq(r, 0);
	g->major = major;
	g->minor = minor;

	r = amdgpu_cs_ctx_create(g->dev, &g->ctx);
	igt_assert_eq(r, 0);

	memset(&g->gpu_info, 0, sizeof(g->gpu_info));
	r = amdgpu_query_gpu_info(g->dev, &g->gpu_info);
	igt_assert_eq(r, 0);
}

static void gpu_close(struct gpu_ctx *g)
{
	if (g->ctx) {
		amdgpu_cs_ctx_free(g->ctx);
		g->ctx = NULL;
	}
	if (g->dev) {
		amdgpu_device_deinitialize(g->dev);
		g->dev = NULL;
	}
	if (g->fd >= 0) {
		close(g->fd);
		g->fd = -1;
	}
}

/*
 * Per-frame fill value so we can detect stale / cross-frame data.
 */
static inline uint32_t frame_pattern(uint32_t frame)
{
	return PATTERN_SEED ^ (frame * 0x9E3779B9u);
}

/*
 * Submit an SDMA IB and optionally wait.
 * Uses direct amdgpu_cs_submit() / amdgpu_cs_wait_fences() to support
 * both synchronous and asynchronous submission (the project's
 * amdgpu_test_exec_cs_helper() always waits synchronously).
 */
static void submit_ib(struct gpu_ctx *g,
		      struct amdgpu_ring_context *ring,
		      bool wait,
		      struct amdgpu_cs_fence *out_fence)
{
	amdgpu_bo_handle ib_bo;
	amdgpu_va_handle ib_va_h;
	uint64_t ib_mc;
	void *ib_cpu;
	struct amdgpu_cs_ib_info ib_info = {};
	struct amdgpu_cs_request req = {};
	amdgpu_bo_list_handle bo_list;
	amdgpu_bo_handle *all_res;
	int r;

	/* Allocate IB buffer (GTT, small) */
	r = amdgpu_bo_alloc_and_map(g->dev, 4096, 4096,
				    AMDGPU_GEM_DOMAIN_GTT, 0,
				    &ib_bo, &ib_cpu, &ib_mc, &ib_va_h);
	igt_assert_eq(r, 0);

	/* Copy PM4 packet into IB BO */
	memcpy(ib_cpu, ring->pm4, ring->pm4_dw * sizeof(uint32_t));

	/* Build bo_list: caller resources + IB BO */
	all_res = alloca(sizeof(amdgpu_bo_handle) * (ring->res_cnt + 1));
	memcpy(all_res, ring->resources,
	       ring->res_cnt * sizeof(amdgpu_bo_handle));
	all_res[ring->res_cnt] = ib_bo;

	r = amdgpu_bo_list_create(g->dev, ring->res_cnt + 1,
				  all_res, NULL, &bo_list);
	igt_assert_eq(r, 0);

	ib_info.ib_mc_address = ib_mc;
	ib_info.size = ring->pm4_dw;

	req.ip_type = AMDGPU_HW_IP_DMA;
	req.ring = 0;
	req.number_of_ibs = 1;
	req.ibs = &ib_info;
	req.resources = bo_list;

	r = amdgpu_cs_submit(g->ctx, 0, &req, 1);
	igt_assert_eq(r, 0);

	if (out_fence) {
		out_fence->context = g->ctx;
		out_fence->ip_type = AMDGPU_HW_IP_DMA;
		out_fence->ip_instance = 0;
		out_fence->ring = 0;
		out_fence->fence = req.seq_no;
	}

	if (wait) {
		struct amdgpu_cs_fence f;
		uint32_t expired = 0;

		f.context = g->ctx;
		f.ip_type = AMDGPU_HW_IP_DMA;
		f.ip_instance = 0;
		f.ring = 0;
		f.fence = req.seq_no;
		r = amdgpu_cs_wait_fences(&f, 1, true,
					  AMDGPU_TIMEOUT_INFINITE,
					  &expired, NULL);
		igt_assert_eq(r, 0);
		igt_assert(expired);
	}

	amdgpu_bo_list_destroy(bo_list);
	amdgpu_bo_unmap_and_free(ib_bo, ib_va_h, ib_mc, 4096);
}

static void wait_fence(struct amdgpu_cs_fence *fence)
{
	uint32_t expired = 0;
	int r;

	r = amdgpu_cs_wait_fences(fence, 1, true,
				  AMDGPU_TIMEOUT_INFINITE,
				  &expired, NULL);
	igt_assert_eq(r, 0);
	igt_assert(expired);
}

/*
 * Read refresh rate from environment or use default.
 */
static int get_refresh_hz(void)
{
	const char *env = getenv("IGT_CROSS_GPU_REFRESH_HZ");
	int hz;

	if (env) {
		hz = atoi(env);
		if (hz > 0 && hz <= 1000)
			return hz;
		igt_warn("IGT_CROSS_GPU_REFRESH_HZ=%s invalid, using %d\n",
			 env, DEFAULT_HZ);
	}
	return DEFAULT_HZ;
}

static int get_num_frames(void)
{
	const char *env = getenv("IGT_CROSS_GPU_NUM_FRAMES");
	int n;

	if (env) {
		n = atoi(env);
		if (n > 0 && n <= 100000)
			return n;
		igt_warn("IGT_CROSS_GPU_NUM_FRAMES=%s invalid, using %d\n",
			 env, DEFAULT_FRAMES);
	}
	return DEFAULT_FRAMES;
}

/*
 * Sleep for one refresh interval (nanosecond precision).
 */
static void vsync_sleep(int refresh_hz)
{
	struct timespec ts;

	ts.tv_sec = 0;
	ts.tv_nsec = 1000000000L / refresh_hz;
	nanosleep(&ts, NULL);
}

/*
 * Allocate a shared VRAM buffer on GPU A, export it as DMA-buf,
 * import on GPU B and VA-map there.
 */
static void setup_shared_dmabuf(struct gpu_ctx *A, struct gpu_ctx *B,
				struct shared_dmabuf *s)
{
	struct amdgpu_bo_import_result import_res = {};
	void *shared_cpu;
	int r;

	/* Allocate shared buffer on GPU A in VRAM */
	r = amdgpu_bo_alloc_and_map(A->dev, BUF_SIZE, 4096,
				    AMDGPU_GEM_DOMAIN_VRAM, 0,
				    &s->shared_bo, &shared_cpu,
				    &s->shared_va, &s->shared_va_h);
	igt_assert_eq(r, 0);

	/* Export from GPU A as DMA-buf */
	r = amdgpu_bo_export(s->shared_bo,
			     amdgpu_bo_handle_type_dma_buf_fd,
			     (uint32_t *)&s->dmabuf_fd);
	igt_assert_eq(r, 0);

	/* Import on GPU B */
	r = amdgpu_bo_import(B->dev,
			     amdgpu_bo_handle_type_dma_buf_fd,
			     (uint32_t)s->dmabuf_fd, &import_res);
	igt_assert_eq(r, 0);
	s->imported_bo = import_res.buf_handle;

	/* VA-map imported BO on GPU B */
	r = amdgpu_va_range_alloc(B->dev, amdgpu_gpu_va_range_general,
				  BUF_SIZE, 4096, 0,
				  &s->imported_va, &s->imported_va_h, 0);
	igt_assert_eq(r, 0);
	r = amdgpu_bo_va_op(s->imported_bo, 0, BUF_SIZE,
			    s->imported_va, 0, AMDGPU_VA_OP_MAP);
	igt_assert_eq(r, 0);
}

/*
 * Tear down shared DMA-buf state: unmap, free, close fd.
 */
static void cleanup_shared_dmabuf(struct shared_dmabuf *s)
{
	if (s->imported_bo) {
		amdgpu_bo_va_op(s->imported_bo, 0, BUF_SIZE,
				s->imported_va, 0, AMDGPU_VA_OP_UNMAP);
		amdgpu_va_range_free(s->imported_va_h);
		amdgpu_bo_free(s->imported_bo);
	}
	if (s->shared_bo)
		amdgpu_bo_unmap_and_free(s->shared_bo, s->shared_va_h,
					 s->shared_va, BUF_SIZE);
	if (s->dmabuf_fd >= 0)
		close(s->dmabuf_fd);
}

/*
 * Core frame-transfer loop.
 *
 * For each frame:
 *   1. GPU A: SDMA const_fill shared buffer with frame pattern.
 *      Uses ip_block->funcs->const_fill() from the project library;
 *      sets funcs->deadbeaf to the per-frame value before the call.
 *   2. Refresh-rate delay (simulates vsync interval).
 *   3. GPU B: SDMA copy_linear imported buffer -> staging.
 *      In implicit mode, GPU B submits without waiting for A's fence
 *      (relies on kernel DMA-buf implicit sync).
 *      In explicit mode, CPU waits for A before B submits.
 *   4. CRC-32 verification of staging vs. expected pattern.
 */
static void cross_gpu_frame_test(struct gpu_ctx *A,
				 struct gpu_ctx *B,
				 const struct amdgpu_ip_block_version *ip_block,
				 struct shared_dmabuf *s,
				 const uint32_t *crc_tbl,
				 bool implicit)
{
	/* C90: all declarations at top */
	amdgpu_bo_handle staging_bo;
	amdgpu_va_handle staging_va_h;
	uint64_t staging_va;
	void *staging_cpu;

	struct amdgpu_ring_context ring_a;
	struct amdgpu_ring_context ring_b;
	uint32_t pm4_a[PM4_DW_MAX];
	uint32_t pm4_b[PM4_DW_MAX];

	int refresh_hz;
	int num_frames;
	int mismatches;
	int frame;
	uint32_t val;
	uint32_t crc_got, crc_exp;
	struct amdgpu_cs_fence fence_a;
	int r;

	refresh_hz = get_refresh_hz();
	num_frames = get_num_frames();
	mismatches = 0;

	igt_info("Cross-GPU frame test: %d frames at %d Hz (%s sync)\n",
		 num_frames, refresh_hz, implicit ? "implicit" : "explicit");

	/* Allocate staging BO on GPU B (GTT, CPU-readable) */
	r = amdgpu_bo_alloc_and_map(B->dev, BUF_SIZE, 4096,
				    AMDGPU_GEM_DOMAIN_GTT, 0,
				    &staging_bo, &staging_cpu,
				    &staging_va, &staging_va_h);
	igt_assert_eq(r, 0);

	/* Prepare ring context for GPU A (const_fill into shared buffer) */
	memset(&ring_a, 0, sizeof(ring_a));
	ring_a.pm4 = pm4_a;
	ring_a.pm4_size = PM4_DW_MAX;
	ring_a.write_length = BUF_SIZE;
	ring_a.bo_mc = s->shared_va;          /* const_fill destination */
	ring_a.resources[0] = s->shared_bo;
	ring_a.res_cnt = 1;

	/* Prepare ring context for GPU B (copy_linear: imported -> staging) */
	memset(&ring_b, 0, sizeof(ring_b));
	ring_b.pm4 = pm4_b;
	ring_b.pm4_size = PM4_DW_MAX;
	ring_b.write_length = BUF_SIZE;
	ring_b.bo_mc = s->imported_va;        /* copy source */
	ring_b.bo_mc2 = staging_va;        /* copy destination */
	ring_b.resources[0] = s->imported_bo;
	ring_b.resources[1] = staging_bo;
	ring_b.res_cnt = 2;

	for (frame = 1; frame <= num_frames; frame++) {
		val = frame_pattern(frame);

		/*
		 * Step 1: GPU A fills shared buffer with per-frame pattern.
		 * Set funcs->deadbeaf to the desired fill value, then call
		 * the project's const_fill() hook which reads it.
		 */
		ip_block->funcs->deadbeaf = val;
		ip_block->funcs->const_fill(ip_block->funcs, &ring_a,
					    &ring_a.pm4_dw);
		submit_ib(A, &ring_a, !implicit, &fence_a);

		/* Step 2: refresh-rate delay (vsync simulation) */
		vsync_sleep(refresh_hz);

		/* Step 3: GPU B copies imported buffer to staging.
		 * Use ip_block->funcs->copy_linear() from the project library
		 * to build the SDMA COPY_LINEAR packet.
		 */
		ip_block->funcs->copy_linear(ip_block->funcs, &ring_b,
					     &ring_b.pm4_dw);
		memset(staging_cpu, 0, BUF_SIZE);
		submit_ib(B, &ring_b, true, NULL);

		/* Step 4: CRC-32 verification */
		crc_got = crc32_buf(crc_tbl, staging_cpu, BUF_SIZE);
		crc_exp = expected_crc32(crc_tbl, val, BUF_SIZE);

		if (crc_got != crc_exp) {
			igt_warn("frame %d: CRC mismatch: "
				 "expected 0x%08x got 0x%08x "
				 "(fill=0x%08x)\n",
				 frame, crc_exp, crc_got, val);
			mismatches++;
			if (mismatches >= 20) {
				igt_warn("Too many CRC mismatches, "
					 "stopping\n");
				break;
			}
		}

		/* Drain GPU A fence so fences don't pile up */
		if (implicit)
			wait_fence(&fence_a);
	}

	igt_assert_f(mismatches == 0,
		     "CRC mismatches in %d of %d frames (%s sync, %d Hz)\n",
		     mismatches, num_frames,
		     implicit ? "implicit" : "explicit",
		     refresh_hz);

	amdgpu_bo_unmap_and_free(staging_bo, staging_va_h,
				 staging_va, BUF_SIZE);
}

int igt_main()
{
	struct gpu_ctx A = { .fd = -1 };
	struct gpu_ctx B = { .fd = -1 };
	const struct amdgpu_ip_block_version *ip_block = NULL;
	struct shared_dmabuf dmabuf = { .dmabuf_fd = -1 };
	uint32_t *crc_tbl = NULL;

	igt_fixture() {
		int fd_a, fd_b;
		int idx = 0;
		int r;

		/* Open two AMDGPU render nodes */
		fd_a = __drm_open_driver_another(idx++, DRIVER_AMDGPU);
		igt_require_f(fd_a >= 0,
			      "Need at least one AMD GPU\n");
		fd_b = __drm_open_driver_another(idx++, DRIVER_AMDGPU);
		if (fd_b < 0) {
			close(fd_a);
			igt_require_f(false,
				      "Need two AMD GPUs for cross-GPU test\n");
		}
		igt_info("GPU A: fd=%d, GPU B: fd=%d\n", fd_a, fd_b);

		gpu_open(&A, fd_a);
		gpu_open(&B, fd_b);

		/* Register IP blocks for GPU A's chip family */
		r = setup_amdgpu_ip_blocks(A.major, A.minor,
					   &A.gpu_info, A.dev);
		igt_assert_eq(r, 0);

		ip_block = get_ip_block(A.dev, AMDGPU_HW_IP_DMA);
		igt_require_f(ip_block != NULL,
			      "SDMA IP block not available\n");
		igt_info("SDMA IP block: family_id=%u\n",
			 ip_block->funcs->family_id);

		setup_shared_dmabuf(&A, &B, &dmabuf);

		crc_tbl = malloc(256 * sizeof(uint32_t));
		igt_assert(crc_tbl);
		crc32_build_table(crc_tbl);
	}

	igt_describe("Cross-GPU implicit sync at configurable refresh rate "
		     "with CRC-32 verification. GPU A fills via SDMA, "
		     "GPU B copies without explicit fence wait.");
	igt_subtest("implicit-sync") {
		cross_gpu_frame_test(&A, &B, ip_block, &dmabuf, crc_tbl, true);
	}

	igt_describe("Cross-GPU explicit sync at configurable refresh rate "
		     "with CRC-32 verification. CPU waits for GPU A "
		     "before GPU B copies. Control test.");
	igt_subtest("explicit-sync") {
		cross_gpu_frame_test(&A, &B, ip_block, &dmabuf, crc_tbl, false);
	}

	igt_fixture() {
		free(crc_tbl);
		cleanup_shared_dmabuf(&dmabuf);
		gpu_close(&B);
		gpu_close(&A);
	}
}
