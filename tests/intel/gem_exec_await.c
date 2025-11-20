/*
 * Copyright © 2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include <signal.h>
#include <sys/ioctl.h>

#include "drmtest.h"
#include "i915/gem.h"
#include "i915/gem_create.h"
#include "i915/gem_engine_topology.h"
#include "i915/gem_mman.h"
#include "i915/gem_submission.h"
#include "i915/gem_vm.h"
#include "igt_aux.h"
#include "igt_core.h"
#include "igt_rand.h"
#include "igt_sysfs.h"
#include "igt_types.h"
#include "igt_vgem.h"
#include "intel_chipset.h"
#include "intel_ctx.h"
#include "intel_gpu_commands.h"
#include "ioctl_wrappers.h"

/**
 * TEST: gem exec await
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: CMD submission
 * Functionality: requests
 * Feature: cmd_submission
 * Test category: GEM_Legacy
 *
 * SUBTEST: wide-all
 *
 * SUBTEST: wide-contexts
 */

static double elapsed(const struct timespec *start, const struct timespec *end)
{
	return ((end->tv_sec - start->tv_sec) +
		(end->tv_nsec - start->tv_nsec)*1e-9);
}

static void xchg_obj(void *array, unsigned i, unsigned j)
{
	struct drm_i915_gem_exec_object2 *obj = array;
	uint64_t tmp;

	tmp = obj[i].handle;
	obj[i].handle = obj[j].handle;
	obj[j].handle = tmp;

	tmp = obj[i].offset;
	obj[i].offset = obj[j].offset;
	obj[j].offset = tmp;
}

#define CONTEXTS 0x1
static void wide(int fd, intel_ctx_cfg_t *cfg, int ring_size,
		 int timeout, unsigned int flags)
{
	const struct intel_execution_engine2 *engine;
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	struct {
		struct drm_i915_gem_exec_object2 *obj;
		struct drm_i915_gem_exec_object2 exec[2];
		struct drm_i915_gem_execbuffer2 execbuf;
		const intel_ctx_t *ctx;
		uint32_t *cmd;
	} *exec;
	struct drm_i915_gem_exec_object2 *obj;
	struct drm_i915_gem_execbuffer2 execbuf;
	unsigned engines[I915_EXEC_RING_MASK + 1], nengine;
	const intel_ctx_t *ctx;
	unsigned long count;
	double time;

	__gem_vm_create(fd, &cfg->vm);
	if (__intel_ctx_create(fd, cfg, &ctx))
		ctx = intel_ctx_0(fd);

	nengine = 0;
	for_each_ctx_engine(fd, ctx, engine) {
		if (!gem_class_has_mutable_submission(fd, engine->class))
			continue;

		engines[nengine++] = engine->flags;
		if (nengine == ARRAY_SIZE(engines))
			break;
	}
	igt_require(nengine);

	exec = calloc(nengine, sizeof(*exec));
	igt_assert(exec);

	igt_require_memory(nengine*(2 + ring_size), 4096, CHECK_RAM);
	obj = calloc(nengine * (ring_size  + 1) + 1, sizeof(*obj));
	igt_assert(obj);

	for (unsigned e = 0; e < nengine; e++) {
		exec[e].obj = calloc(ring_size, sizeof(*exec[e].obj));
		igt_assert(exec[e].obj);
		for (unsigned n = 0; n < ring_size; n++)  {
			exec[e].obj[n].handle = gem_create(fd, 4096);
			exec[e].obj[n].flags = EXEC_OBJECT_WRITE;
			obj[e * ring_size + n] = exec[e].obj[n];
		}

		exec[e].execbuf.buffers_ptr = to_user_pointer(exec[e].exec);
		exec[e].execbuf.buffer_count = 2;
		exec[e].execbuf.flags = engines[e];
		exec[e].execbuf.rsvd1 = ctx->id;

		if (flags & CONTEXTS) {
			exec[e].ctx = intel_ctx_create(fd, cfg);
			exec[e].execbuf.rsvd1 = exec[e].ctx->id;
		}

		exec[e].exec[1].handle = gem_create(fd, 4096);
		obj[nengine * ring_size + e] = exec[e].exec[1];
	}

	obj[nengine * (ring_size + 1)].handle = gem_create(fd, 4096);
	gem_write(fd, obj[nengine * (ring_size + 1)].handle, 0,
		  &bbe, sizeof(bbe));

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = nengine * (ring_size + 1) + 1;
	execbuf.rsvd1 = ctx->id;
	gem_execbuf(fd, &execbuf); /* tag the object as a batch in the GTT */
	for (unsigned e = 0; e < nengine; e++) {
		uint64_t address;
		uint32_t *cs;

		for (unsigned n = 0; n < ring_size; n++) {
			obj[e * ring_size + n].flags |= EXEC_OBJECT_PINNED;
			exec[e].obj[n] = obj[e * ring_size + n];
		}
		exec[e].exec[1] = obj[nengine * ring_size + e];
		exec[e].exec[1].flags |= EXEC_OBJECT_PINNED;
		address = exec[e].exec[1].offset;

		exec[e].cmd = gem_mmap__device_coherent(fd, exec[e].exec[1].handle,
							0, 4096, PROT_WRITE);
		cs = exec[e].cmd;

		*cs++ = MI_NOOP; /* placeholder for MI_ARB_CHECK */
		if (gen >= 8) {
			*cs++ = MI_BATCH_BUFFER_START | 1 << 8 | 1;
			*cs++ = address;
			*cs++ = address >> 32;
		} else if (gen >= 6) {
			*cs++ = MI_BATCH_BUFFER_START | 1 << 8;
			*cs++ = address;
		} else {
			*cs++ = MI_BATCH_BUFFER_START | 2 << 6;
			if (gen < 4)
				address |= 1;
			*cs++ = address;
		}
	}

	intel_detect_and_clear_missed_interrupts(fd);

	time = 0;
	count = 0;
	igt_until_timeout(timeout) {
		struct timespec start, now;
		for (unsigned e = 0; e < nengine; e++) {
			if (flags & CONTEXTS) {
				intel_ctx_destroy(fd, exec[e].ctx);
				exec[e].ctx = intel_ctx_create(fd, cfg);
				exec[e].execbuf.rsvd1 = exec[e].ctx->id;
			}

			gem_set_domain(fd, exec[e].exec[1].handle,
				       I915_GEM_DOMAIN_WC, I915_GEM_DOMAIN_WC);
			exec[e].cmd[0] = MI_ARB_CHECK;

			exec[e].exec[0] = obj[nengine * (ring_size + 1)];
			gem_execbuf(fd, &exec[e].execbuf);

			for (unsigned n = 0; n < ring_size; n++) {
				exec[e].exec[0] = exec[e].obj[n];
				gem_execbuf(fd, &exec[e].execbuf);
			}
		}

		igt_permute_array(obj, nengine*ring_size, xchg_obj);

		clock_gettime(CLOCK_MONOTONIC, &start);
		for (unsigned e = 0; e < nengine; e++) {
			execbuf.flags = engines[e];
			gem_execbuf(fd, &execbuf);
		}
		clock_gettime(CLOCK_MONOTONIC, &now);
		time += elapsed(&start, &now);
		count += nengine;

		for (unsigned e = 0; e < nengine; e++)
			exec[e].cmd[0] = MI_BATCH_BUFFER_END;
		__sync_synchronize();
	}

	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);

	igt_info("%s: %'lu cycles: %.3fus\n",
		 __func__, count, time*1e6 / count);

	for (unsigned n = 0; n < nengine * (ring_size + 1) + 1; n++)
		gem_close(fd, obj[n].handle);
	free(obj);

	for (unsigned e = 0; e < nengine; e++) {
		if (flags & CONTEXTS)
			intel_ctx_destroy(fd, exec[e].ctx);

		munmap(exec[e].cmd, 4096);
		free(exec[e].obj);
	}
	free(exec);

	intel_ctx_destroy(fd, ctx);
	__gem_vm_destroy(fd, cfg->vm);
	cfg->vm = 0;
}

#define TIMEOUT 20

igt_main
{
	intel_ctx_cfg_t cfg;
	int ring_size = 0;
	igt_fd_t(device);

	igt_fixture() {

		device = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(device);
		gem_submission_print_method(device);
		cfg = intel_ctx_cfg_all_physical(device);

		ring_size = gem_submission_measure(device, &cfg, ALL_ENGINES);

		igt_info("Ring size: %d batches\n", ring_size);
		igt_require(ring_size > 0);

		igt_fork_hang_detector(device);
	}

	igt_subtest("wide-all")
		wide(device, &cfg, ring_size, TIMEOUT, 0);

	igt_subtest("wide-contexts") {
		gem_require_contexts(device);
		wide(device, &cfg, ring_size, TIMEOUT, CONTEXTS);
	}

	igt_fixture() {
		igt_stop_hang_detector();
	}
}
