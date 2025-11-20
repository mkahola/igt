/*
 * Copyright © 2022 Intel Corporation
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

#include <fcntl.h>
#include <sys/ioctl.h>

#include "igt.h"
#include "igt_core.h"
#include "igt_device.h"
#include "igt_drm_fdinfo.h"
#include "i915/gem.h"
#include "i915/gem_create.h"
#include "i915/gem_vm.h"
#include "intel_ctx.h"
/**
 * TEST: i915 drm fdinfo
 * Description: Test the i915 drm fdinfo data
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: driver
 * Functionality: Per client memory statistics
 * Feature: client_busyness
 *
 * SUBTEST: all-busy-check-all
 *
 * SUBTEST: all-busy-idle-check-all
 *
 * SUBTEST: basics
 *
 * SUBTEST: busy
 *
 * SUBTEST: busy-check-all
 *
 * SUBTEST: busy-hang
 *
 * SUBTEST: busy-idle
 *
 * SUBTEST: busy-idle-check-all
 *
 * SUBTEST: idle
 *
 * SUBTEST: isolation
 *
 * SUBTEST: most-busy-check-all
 *
 * SUBTEST: most-busy-idle-check-all
 *
 * SUBTEST: virtual-busy
 *
 * SUBTEST: virtual-busy-all
 *
 * SUBTEST: virtual-busy-hang
 *
 * SUBTEST: virtual-busy-hang-all
 *
 * SUBTEST: virtual-busy-idle
 *
 * SUBTEST: virtual-busy-idle-all
 *
 * SUBTEST: virtual-idle
 *
 * SUBTEST: memory-info-idle
 *
 * SUBTEST: memory-info-active
 *
 * SUBTEST: memory-info-resident
 *
 * SUBTEST: memory-info-purgeable
 *
 * SUBTEST: memory-info-shared
 *
 * SUBTEST: context-close-stress
 */

IGT_TEST_DESCRIPTION("Test the i915 drm fdinfo data");

const double tolerance = 0.05f;
const unsigned long batch_duration_ns = 500e6;

static const char *engine_map[] = {
	"render",
	"copy",
	"video",
	"video-enhance",
	"compute",
};

static void basics(int i915, unsigned int num_classes)
{
	struct drm_client_fdinfo info = { };
	unsigned int ret;

	ret = igt_parse_drm_fdinfo(i915, &info, engine_map,
				   ARRAY_SIZE(engine_map), NULL, 0);
	igt_assert(ret);

	igt_assert(!strcmp(info.driver, "i915"));

	igt_assert_eq(info.num_engines, num_classes);
}

#define TEST_BUSY (1)
#define FLAG_SYNC (2)
#define TEST_TRAILING_IDLE (4)
#define FLAG_HANG (8)
#define TEST_ISOLATION (16)

#define TEST_ACTIVE TEST_BUSY
#define TEST_RESIDENT (32)
#define TEST_PURGEABLE (64)
#define TEST_SHARED (128)

static void end_spin(int fd, igt_spin_t *spin, unsigned int flags)
{
	if (!spin)
		return;

	igt_spin_end(spin);

	if (flags & FLAG_SYNC)
		gem_sync(fd, spin->handle);

	if (flags & TEST_TRAILING_IDLE) {
		unsigned long t, timeout = 0;
		struct timespec start = { };

		igt_nsec_elapsed(&start);

		do {
			t = igt_nsec_elapsed(&start);

			if (gem_bo_busy(fd, spin->handle) &&
			    (t - timeout) > 10e6) {
				timeout = t;
				igt_warn("Spinner not idle after %.2fms\n",
					 (double)t / 1e6);
			}

			usleep(1e3);
		} while (t < batch_duration_ns / 5);
	}
}

static uint64_t read_engine_time(int i915, unsigned int class)
{
	struct drm_client_fdinfo info = { };

	igt_assert(igt_parse_drm_fdinfo(i915, &info, engine_map,
					ARRAY_SIZE(engine_map), NULL, 0));

	return info.engine_time[class];
}

static void
single(int gem_fd, const intel_ctx_t *ctx,
       const struct intel_execution_engine2 *e, unsigned int flags)
{
	unsigned long slept;
	igt_spin_t *spin;
	uint64_t val;
	int spin_fd;
	uint64_t ahnd;

	gem_quiescent_gpu(gem_fd);

	if (flags & TEST_BUSY)
		igt_require(!gem_using_guc_submission(gem_fd));

	if (flags & TEST_ISOLATION) {
		spin_fd = drm_reopen_driver(gem_fd);
		ctx = intel_ctx_create_all_physical(spin_fd);
	} else {
		spin_fd = gem_fd;
	}

	ahnd = get_reloc_ahnd(spin_fd, ctx->id);

	if (flags & TEST_BUSY)
		spin = igt_sync_spin(spin_fd, ahnd, ctx, e);
	else
		spin = NULL;

	val = read_engine_time(gem_fd, e->class);
	slept = igt_measured_usleep(batch_duration_ns / 1000) * NSEC_PER_USEC;
	if (flags & TEST_TRAILING_IDLE)
		end_spin(spin_fd, spin, flags);
	val = read_engine_time(gem_fd, e->class) - val;

	if (flags & FLAG_HANG)
		igt_force_gpu_reset(spin_fd);
	else
		end_spin(spin_fd, spin, FLAG_SYNC);

	assert_within_epsilon((flags & TEST_BUSY) && !(flags & TEST_ISOLATION) ? val : slept - val,
			      slept, tolerance);

	/* Check for idle after hang. */
	if (flags & FLAG_HANG) {
		gem_quiescent_gpu(spin_fd);
		igt_assert(!gem_bo_busy(spin_fd, spin->handle));

		val = read_engine_time(gem_fd, e->class);
		slept = igt_measured_usleep(batch_duration_ns / 1000) * NSEC_PER_USEC;
		val = read_engine_time(gem_fd, e->class) - val;

		assert_within_epsilon(slept - val, slept, tolerance);
	}

	igt_spin_free(spin_fd, spin);
	put_ahnd(ahnd);

	gem_quiescent_gpu(spin_fd);
}

static void log_busy(unsigned int num_engines, uint64_t *val)
{
	char buf[1024];
	int rem = sizeof(buf);
	unsigned int i;
	char *p = buf;

	for (i = 0; i < num_engines; i++) {
		int len;

		len = snprintf(p, rem, "%u=%" PRIu64 "\n",  i, val[i]);
		igt_assert_lt(0, len);
		rem -= len;
		p += len;
	}

	igt_info("%s", buf);
}

static void read_engine_time_all(int i915, uint64_t *val)
{
	struct drm_client_fdinfo info = { };

	igt_assert(igt_parse_drm_fdinfo(i915, &info, engine_map,
					ARRAY_SIZE(engine_map), NULL, 0));

	memcpy(val, info.engine_time, sizeof(info.engine_time));
}

static void
busy_check_all(int gem_fd, const intel_ctx_t *ctx,
	       const struct intel_execution_engine2 *e,
	       const unsigned int num_engines,
	       const unsigned int classes[16], const unsigned int num_classes,
	       unsigned int flags)
{
	uint64_t ahnd = get_reloc_ahnd(gem_fd, ctx->id);
	uint64_t tval[2][16];
	unsigned long slept;
	uint64_t val[16];
	igt_spin_t *spin;
	unsigned int i;

	igt_require(!gem_using_guc_submission(gem_fd));

	memset(tval, 0, sizeof(tval));

	spin = igt_sync_spin(gem_fd, ahnd, ctx, e);

	read_engine_time_all(gem_fd, tval[0]);
	slept = igt_measured_usleep(batch_duration_ns / 1000) * NSEC_PER_USEC;
	if (flags & TEST_TRAILING_IDLE)
		end_spin(gem_fd, spin, flags);
	read_engine_time_all(gem_fd, tval[1]);

	end_spin(gem_fd, spin, FLAG_SYNC);
	igt_spin_free(gem_fd, spin);
	put_ahnd(ahnd);

	for (i = 0; i < num_classes; i++)
		val[i] = tval[1][i] - tval[0][i];

	log_busy(num_classes, val);

	for (i = 0; i < num_classes; i++)
		assert_within_epsilon(i == e->class ? val[i] : slept - val[i],
				      slept, tolerance);

	gem_quiescent_gpu(gem_fd);
}

static void
__submit_spin(int gem_fd, igt_spin_t *spin,
	      const struct intel_execution_engine2 *e,
	      int offset)
{
	struct drm_i915_gem_execbuffer2 eb = spin->execbuf;

	eb.flags &= ~(0x3f | I915_EXEC_BSD_MASK);
	eb.flags |= e->flags | I915_EXEC_NO_RELOC;
	eb.batch_start_offset += offset;

	gem_execbuf(gem_fd, &eb);
}

static void
most_busy_check_all(int gem_fd, const intel_ctx_t *ctx,
		    const struct intel_execution_engine2 *e,
		    const unsigned int num_engines,
		    const unsigned int classes[16],
		    const unsigned int num_classes,
		    unsigned int flags)
{
	uint64_t ahnd = get_reloc_ahnd(gem_fd, ctx->id);
	unsigned int busy_class[num_classes];
	struct intel_execution_engine2 *e_;
	igt_spin_t *spin = NULL;
	uint64_t tval[2][16];
	unsigned long slept;
	uint64_t val[16];
	unsigned int i;

	igt_require(!gem_using_guc_submission(gem_fd));

	memset(busy_class, 0, sizeof(busy_class));
	memset(tval, 0, sizeof(tval));

	gem_quiescent_gpu(gem_fd);

	for_each_ctx_engine(gem_fd, ctx, e_) {
		if (e->class == e_->class && e->instance == e_->instance) {
			continue;
		} else if (spin) {
			__submit_spin(gem_fd, spin, e_, 64);
			busy_class[e_->class]++;
		} else {
			spin = __igt_sync_spin_poll(gem_fd, ahnd, ctx, e_);
			busy_class[e_->class]++;
		}
	}
	igt_require(spin); /* at least one busy engine */

	/* Small delay to allow engines to start. */
	usleep(__igt_sync_spin_wait(gem_fd, spin) * num_engines / 1e3);

	read_engine_time_all(gem_fd, tval[0]);
	slept = igt_measured_usleep(batch_duration_ns / 1000) * NSEC_PER_USEC;
	if (flags & TEST_TRAILING_IDLE)
		end_spin(gem_fd, spin, flags);
	read_engine_time_all(gem_fd, tval[1]);

	end_spin(gem_fd, spin, FLAG_SYNC);
	igt_spin_free(gem_fd, spin);
	put_ahnd(ahnd);

	for (i = 0; i < num_classes; i++)
		val[i] = tval[1][i] - tval[0][i];

	log_busy(num_classes, val);

	for (i = 0; i < num_classes; i++) {
		double target = slept * busy_class[i] ?: slept;

		assert_within_epsilon(busy_class[i] ? val[i] : slept - val[i],
				      target, tolerance);
	}
	gem_quiescent_gpu(gem_fd);
}

static void
all_busy_check_all(int gem_fd, const intel_ctx_t *ctx,
		   const unsigned int num_engines,
		   const unsigned int classes[16],
		   const unsigned int num_classes,
		   unsigned int flags)
{
	uint64_t ahnd = get_reloc_ahnd(gem_fd, ctx->id);
	unsigned int busy_class[num_classes];
	struct intel_execution_engine2 *e;
	igt_spin_t *spin = NULL;
	uint64_t tval[2][16];
	unsigned long slept;
	uint64_t val[16];
	unsigned int i;

	igt_require(!gem_using_guc_submission(gem_fd));

	memset(busy_class, 0, sizeof(busy_class));
	memset(tval, 0, sizeof(tval));

	for_each_ctx_engine(gem_fd, ctx, e) {
		if (spin)
			__submit_spin(gem_fd, spin, e, 64);
		else
			spin = __igt_sync_spin_poll(gem_fd, ahnd, ctx, e);
		busy_class[e->class]++;
	}

	/* Small delay to allow engines to start. */
	usleep(__igt_sync_spin_wait(gem_fd, spin) * num_engines / 1e3);

	read_engine_time_all(gem_fd, tval[0]);
	slept = igt_measured_usleep(batch_duration_ns / 1000) * NSEC_PER_USEC;
	if (flags & TEST_TRAILING_IDLE)
		end_spin(gem_fd, spin, flags);
	read_engine_time_all(gem_fd, tval[1]);

	end_spin(gem_fd, spin, FLAG_SYNC);
	igt_spin_free(gem_fd, spin);
	put_ahnd(ahnd);

	for (i = 0; i < num_classes; i++)
		val[i] = tval[1][i] - tval[0][i];

	log_busy(num_classes, val);

	for (i = 0; i < num_classes; i++) {
		double target = slept * busy_class[i] ?: slept;

		assert_within_epsilon(busy_class[i] ? val[i] : slept - val[i],
				      target, tolerance);
	}
	gem_quiescent_gpu(gem_fd);
}

static struct i915_engine_class_instance *
list_engines(const intel_ctx_cfg_t *cfg,
	     unsigned int class, unsigned int *out)
{
	struct i915_engine_class_instance *ci;
	unsigned int count = 0, i;

	ci = malloc(cfg->num_engines * sizeof(*ci));
	igt_assert(ci);

	for (i = 0; i < cfg->num_engines; i++) {
		if (class == cfg->engines[i].engine_class)
			ci[count++] = cfg->engines[i];
	}

	if (!count) {
		free(ci);
		ci = NULL;
	}

	*out = count;
	return ci;
}

static size_t sizeof_load_balance(int count)
{
	return offsetof(struct i915_context_engines_load_balance,
			engines[count]);
}

static size_t sizeof_param_engines(int count)
{
	return offsetof(struct i915_context_param_engines,
			engines[count]);
}

#define alloca0(sz) ({ size_t sz__ = (sz); memset(alloca(sz__), 0, sz__); })

static int __set_load_balancer(int i915, uint32_t ctx,
			       const struct i915_engine_class_instance *ci,
			       unsigned int count,
			       void *ext)
{
	struct i915_context_engines_load_balance *balancer =
		alloca0(sizeof_load_balance(count));
	struct i915_context_param_engines *engines =
		alloca0(sizeof_param_engines(count + 1));
	struct drm_i915_gem_context_param p = {
		.ctx_id = ctx,
		.param = I915_CONTEXT_PARAM_ENGINES,
		.size = sizeof_param_engines(count + 1),
		.value = to_user_pointer(engines)
	};

	balancer->base.name = I915_CONTEXT_ENGINES_EXT_LOAD_BALANCE;
	balancer->base.next_extension = to_user_pointer(ext);

	igt_assert(count);
	balancer->num_siblings = count;
	memcpy(balancer->engines, ci, count * sizeof(*ci));

	engines->extensions = to_user_pointer(balancer);
	engines->engines[0].engine_class =
		I915_ENGINE_CLASS_INVALID;
	engines->engines[0].engine_instance =
		I915_ENGINE_CLASS_INVALID_NONE;
	memcpy(engines->engines + 1, ci, count * sizeof(*ci));

	return __gem_context_set_param(i915, &p);
}

static void set_load_balancer(int i915, uint32_t ctx,
			      const struct i915_engine_class_instance *ci,
			      unsigned int count,
			      void *ext)
{
	igt_assert_eq(__set_load_balancer(i915, ctx, ci, count, ext), 0);
}

static void
virtual(int i915, const intel_ctx_cfg_t *base_cfg, unsigned int flags)
{
	intel_ctx_cfg_t cfg = {};

	if (flags & TEST_BUSY)
		igt_require(!gem_using_guc_submission(i915));

	cfg.vm = gem_vm_create(i915);

	for (int class = 0; class < 32; class++) {
		struct i915_engine_class_instance *ci;
		unsigned int count;

		if (!gem_class_can_store_dword(i915, class))
			continue;

		ci = list_engines(base_cfg, class, &count);
		if (!ci)
			continue;

		for (unsigned int pass = 0; pass < count; pass++) {
			const intel_ctx_t *ctx;
			unsigned long slept;
			uint64_t ahnd, val;
			igt_spin_t *spin;
			igt_hang_t hang;

			igt_assert(sizeof(*ci) == sizeof(int));
			igt_permute_array(ci, count, igt_exchange_int);

			igt_debug("class %u, pass %u/%u...\n", class, pass, count);

			ctx = intel_ctx_create(i915, &cfg);
			set_load_balancer(i915, ctx->id, ci, count, NULL);
			if (flags & FLAG_HANG)
				hang = igt_allow_hang(i915, ctx->id, 0);
			ahnd = get_reloc_ahnd(i915, ctx->id);

			if (flags & TEST_BUSY)
				spin = igt_sync_spin(i915, ahnd, ctx, NULL);
			else
				spin = NULL;

			val = read_engine_time(i915, class);
			slept = igt_measured_usleep(batch_duration_ns / 1000) * NSEC_PER_USEC;
			if (flags & TEST_TRAILING_IDLE)
				end_spin(i915, spin, flags);
			val = read_engine_time(i915, class) - val;

			if (flags & FLAG_HANG)
				igt_force_gpu_reset(i915);
			else
				end_spin(i915, spin, FLAG_SYNC);

			assert_within_epsilon(flags & TEST_BUSY ? val : slept - val,
					      slept, tolerance);

			/* Check for idle after hang. */
			if (flags & FLAG_HANG) {
				gem_quiescent_gpu(i915);
				igt_assert(!gem_bo_busy(i915, spin->handle));

				val = read_engine_time(i915, class);
				slept = igt_measured_usleep(batch_duration_ns /
							1000) * NSEC_PER_USEC;
				val = read_engine_time(i915, class) - val;

				assert_within_epsilon(slept - val, slept, tolerance);
			}

			igt_spin_free(i915, spin);
			put_ahnd(ahnd);
			if (flags & FLAG_HANG)
				igt_disallow_hang(i915, hang);
			intel_ctx_destroy(i915, ctx);

			gem_quiescent_gpu(i915);
		}

		free(ci);
	}
}

static void
__virt_submit_spin(int i915, igt_spin_t *spin,
		   const intel_ctx_t *ctx,
		   int offset)
{
	struct drm_i915_gem_execbuffer2 eb = spin->execbuf;

	eb.flags &= ~(0x3f | I915_EXEC_BSD_MASK);
	eb.flags |= I915_EXEC_NO_RELOC;
	eb.batch_start_offset += offset;
	eb.rsvd1 = ctx->id;

	gem_execbuf(i915, &eb);
}

static void
virtual_all(int i915, const intel_ctx_cfg_t *base_cfg, unsigned int flags)
{
	const unsigned int num_engines = base_cfg->num_engines;
	intel_ctx_cfg_t cfg = {};

	igt_require(!gem_using_guc_submission(i915));

	cfg.vm = gem_vm_create(i915);

	for (int class = 0; class < 32; class++) {
		struct i915_engine_class_instance *ci;
		const intel_ctx_t *ctx[num_engines];
		igt_hang_t hang[num_engines];
		igt_spin_t *spin = NULL;
		unsigned int count;
		unsigned long slept;
		uint64_t val;

		if (!gem_class_can_store_dword(i915, class))
			continue;

		ci = list_engines(base_cfg, class, &count);
		if (!ci)
			continue;
		igt_assert(count <= num_engines);

		if (count < 2)
			continue;

		igt_debug("class %u, %u engines...\n", class, count);

		for (unsigned int i = 0; i < count; i++) {
			uint64_t ahnd;

			igt_assert(sizeof(*ci) == sizeof(int));
			igt_permute_array(ci, count, igt_exchange_int);

			ctx[i] = intel_ctx_create(i915, &cfg);
			set_load_balancer(i915, ctx[i]->id, ci, count, NULL);
			if (flags & FLAG_HANG)
				hang[i] = igt_allow_hang(i915, ctx[i]->id, 0);
			ahnd = get_reloc_ahnd(i915, ctx[i]->id);

			if (spin)
				__virt_submit_spin(i915, spin, ctx[i], 64);
			else
				spin = __igt_sync_spin_poll(i915, ahnd, ctx[i],
							    NULL);
		}

		/* Small delay to allow engines to start. */
		usleep(__igt_sync_spin_wait(i915, spin) * count / 1e3);

		val = read_engine_time(i915, class);
		slept = igt_measured_usleep(batch_duration_ns / 1000) * NSEC_PER_USEC;
		if (flags & TEST_TRAILING_IDLE)
			end_spin(i915, spin, flags);
		val = read_engine_time(i915, class) - val;

		if (flags & FLAG_HANG)
			igt_force_gpu_reset(i915);
		else
			end_spin(i915, spin, FLAG_SYNC);

		assert_within_epsilon(val, slept * count, tolerance);

		/* Check for idle after hang. */
		if (flags & FLAG_HANG) {
			gem_quiescent_gpu(i915);
			igt_assert(!gem_bo_busy(i915, spin->handle));

			val = read_engine_time(i915, class);
			slept = igt_measured_usleep(batch_duration_ns /
						1000) * NSEC_PER_USEC;
			val = read_engine_time(i915, class) - val;

			assert_within_epsilon(slept - val, slept, tolerance);
		}

		igt_spin_free(i915, spin);
		put_ahnd(spin->opts.ahnd);
		for (unsigned int i = 0; i < count; i++) {
			if (flags & FLAG_HANG)
				igt_disallow_hang(i915, hang[i]);
			intel_ctx_destroy(i915, ctx[i]);
		}

		gem_quiescent_gpu(i915);

		free(ci);
	}
}

static void stress_context_close(int i915)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct igt_helper_process reader = { };
	struct drm_client_fdinfo info;
	uint32_t batch;
	int dir, ret;
	char buf[64];

	ret = snprintf(buf, sizeof(buf), "%u", i915);
	igt_assert(ret > 0 && ret < sizeof(buf));

	dir = open("/proc/self/fdinfo", O_DIRECTORY | O_RDONLY);
	igt_assert_fd(dir);

	memset(&info, 0, sizeof(info));
	ret = __igt_parse_drm_fdinfo(dir, buf, &info, NULL, 0, NULL, 0);
	igt_assert(ret > 0);
	igt_require(info.num_regions);

	batch = gem_create(i915, 4096);
	gem_write(i915, batch, 0, &bbe, sizeof(bbe));

	igt_fork_helper(&reader) {
		for (;;) {
			memset(&info, 0, sizeof(info));
			ret = __igt_parse_drm_fdinfo(dir, buf, &info,
						     NULL, 0, NULL, 0);
			igt_assert(ret > 0);
		}
	}

	igt_until_timeout(10) {
		struct drm_i915_gem_exec_object2 obj = {
			.handle = batch,
		};
		struct drm_i915_gem_execbuffer2 eb = {
			.buffers_ptr = to_user_pointer(&obj),
			.buffer_count = 1,
		};

		eb.rsvd1 = gem_context_create(i915);
		igt_assert(eb.rsvd1);
		gem_execbuf(i915, &eb);
		gem_context_destroy(i915, eb.rsvd1);
	}

	igt_stop_helper(&reader);
}

static size_t read_fdinfo(char *buf, const size_t sz, int at, const char *name)
{
	size_t count;
	int fd;

	fd = openat(at, name, O_RDONLY);
	if (fd < 0)
		return 0;

	count = read(fd, buf, sz - 1);
	if (count > 0)
		buf[count - 1] = 0;
	close(fd);

	return max_t(typeof(count), count, 0);
}

/*
 * At least this much, but maybe less if we started with a driver internal
 * baseline which can go away behind our back.
 */
#define fdinfo_assert_gte(cur, prev, sz, base) \
({ \
	int64_t __sz = (sz) - (base); \
	int64_t __d = (cur) - (prev); \
	igt_assert_f(__d >= __sz, \
		     "prev=%"PRIu64" cur=%"PRIu64" delta=%"PRId64" sz=%"PRIu64" baseline=%"PRIu64"\n%s\n", \
		     (prev), (cur), __d, (sz), (base), fdinfo_buf); \
})

#define fdinfo_assert_eq(cur, prev, sz, base) \
({ \
	int64_t __d = (cur) - (prev); \
	igt_assert_f(__d == 0, \
		     "prev=%"PRIu64" cur=%"PRIu64" delta=%"PRId64" sz=%"PRIu64" baseline=%"PRIu64"\n%s\n", \
		     (prev), (cur), __d, (sz), (base), fdinfo_buf); \
})

static void
test_memory(int i915, struct gem_memory_region *mr, unsigned int flags)
{
	const unsigned int r = mr->ci.memory_class == I915_MEMORY_CLASS_SYSTEM ? 0 : 1; /* See region map */
	const uint64_t max_mem = 512ull * 1024 * 1024;
	const uint64_t max_bo = 16ull * 1024 * 1024;
	struct drm_client_fdinfo base_info, prev_info = { };
	struct drm_client_fdinfo info = { };
	char buf[64], fdinfo_buf[4096];
	igt_spin_t *spin = NULL;
	uint64_t total = 0, sz;
	uint64_t ahnd;
	int ret, dir;

	i915 = drm_reopen_driver(i915);

	ahnd = get_reloc_ahnd(i915, 0);

	ret = snprintf(buf, sizeof(buf), "%u", i915);
	igt_assert(ret > 0 && ret < sizeof(buf));

	dir = open("/proc/self/fdinfo", O_DIRECTORY | O_RDONLY);
	igt_assert_fd(dir);

	gem_quiescent_gpu(i915);
	ret =  __igt_parse_drm_fdinfo(dir, buf, &info, NULL, 0, NULL, 0);
	igt_assert_lt(0, ret);
	igt_require(info.num_regions);
	memcpy(&prev_info, &info, sizeof(info));
	memcpy(&base_info, &info, sizeof(info));

	while (total < max_mem) {
		static const char *region_map[] = {
			"system0",
			"local0",
		};
		uint32_t bo;

		sz = random() % max_bo;
		ret = __gem_create_in_memory_region_list(i915, &bo, &sz, 0,
							 &mr->ci, 1);
		igt_assert_eq(ret, 0);
		total += sz;

		if (flags & (TEST_RESIDENT | TEST_PURGEABLE | TEST_ACTIVE))
			spin = igt_spin_new(i915,
					    .dependency = bo,
					    .ahnd = ahnd);
		else
			spin = NULL;

		if (flags & TEST_PURGEABLE) {
			gem_madvise(i915, bo, I915_MADV_DONTNEED);
			igt_spin_free(i915, spin);
			gem_quiescent_gpu(i915);
			spin = NULL;
		}

		if (flags & TEST_SHARED) {
			struct drm_gem_open open_struct;
			struct drm_gem_flink flink;

			flink.handle = bo;
			ret = ioctl(i915, DRM_IOCTL_GEM_FLINK, &flink);
			igt_assert_eq(ret, 0);

			open_struct.name = flink.name;
			ret = ioctl(i915, DRM_IOCTL_GEM_OPEN, &open_struct);
			igt_assert_eq(ret, 0);
			igt_assert(open_struct.handle != 0);
		}

		memset(&info, 0, sizeof(info));
		ret =  __igt_parse_drm_fdinfo(dir, buf, &info,
					      NULL, 0,
					      region_map, ARRAY_SIZE(region_map));
		igt_assert_lt(0, ret);
		igt_assert(info.num_regions);

		read_fdinfo(fdinfo_buf, sizeof(fdinfo_buf), dir, buf);

		/* >= to account for objects out of our control */
		fdinfo_assert_gte(info.region_mem[r].total,
				  prev_info.region_mem[r].total,
				  sz,
				  base_info.region_mem[r].total);

		if (flags & TEST_SHARED)
			fdinfo_assert_gte(info.region_mem[r].shared,
					  prev_info.region_mem[r].shared,
					  sz,
					  base_info.region_mem[r].shared);
		else
			fdinfo_assert_eq(info.region_mem[r].shared,
					 prev_info.region_mem[r].shared,
					 sz,
					 base_info.region_mem[r].shared);

		if (flags & (TEST_RESIDENT | TEST_PURGEABLE | TEST_ACTIVE))
			fdinfo_assert_gte(info.region_mem[r].resident,
					  (uint64_t)0, /* We can only be sure the current buffer is resident. */
					  sz,
					  (uint64_t)0);

		if (flags & TEST_PURGEABLE)
			fdinfo_assert_gte(info.region_mem[r].purgeable,
					  (uint64_t)0, /* We can only be sure the current buffer is purgeable (subset of resident). */
					  sz,
					  (uint64_t)0);

		if (flags & TEST_ACTIVE)
			fdinfo_assert_gte(info.region_mem[r].active,
					  (uint64_t)0, /* We can only be sure the current buffer is active. */
					  sz,
					  (uint64_t)0);

		memcpy(&prev_info, &info, sizeof(info));

		if (spin) {
			igt_spin_free(i915, spin);
			gem_quiescent_gpu(i915);
		}
	}

	put_ahnd(ahnd);
	close(i915);
}

#define test_each_engine(T, i915, ctx, e) \
	igt_subtest_with_dynamic(T) for_each_ctx_engine(i915, ctx, e) \
		igt_dynamic_f("%s", e->name)

igt_main()
{
	unsigned int num_engines = 0, num_classes = 0;
	const struct intel_execution_engine2 *e;
	unsigned int classes[16] = { };
	const intel_ctx_t *ctx = NULL;
	int i915 = -1;

	igt_fixture() {
		struct drm_client_fdinfo info = { };
		unsigned int i;

		i915 = __drm_open_driver(DRIVER_INTEL);

		igt_require_gem(i915);
		igt_require(igt_parse_drm_fdinfo(i915, &info, NULL, 0, NULL, 0));
		igt_require(info.num_engines);

		ctx = intel_ctx_create_all_physical(i915);

		for_each_ctx_engine(i915, ctx, e) {
			num_engines++;
			igt_assert(e->class < ARRAY_SIZE(classes));
			classes[e->class]++;
		}
		igt_require(num_engines);

		for (i = 0; i < ARRAY_SIZE(classes); i++) {
			if (classes[i])
				num_classes++;
		}
		igt_assert(num_classes);
	}

	/**
	 * Test basic fdinfo content.
	 */
	igt_subtest("basics")
		basics(i915, num_classes);

	/**
	 * Test that engines show no load when idle.
	 */
	test_each_engine("idle", i915, ctx, e)
		single(i915, ctx, e, 0);

	igt_subtest("virtual-idle")
		virtual(i915, &ctx->cfg, 0);

	/**
	 * Test that a single engine reports load correctly.
	 */
	test_each_engine("busy", i915, ctx, e)
		single(i915, ctx, e, TEST_BUSY);

	igt_subtest("virtual-busy")
		virtual(i915, &ctx->cfg, TEST_BUSY);

	test_each_engine("busy-idle", i915, ctx, e)
		single(i915, ctx, e, TEST_BUSY | TEST_TRAILING_IDLE);

	igt_subtest("virtual-busy-idle")
		virtual(i915, &ctx->cfg, TEST_BUSY | TEST_TRAILING_IDLE);

	test_each_engine("busy-hang", i915, ctx, e) {
		igt_hang_t hang = igt_allow_hang(i915, ctx->id, 0);

		single(i915, ctx, e, TEST_BUSY | FLAG_HANG);

		igt_disallow_hang(i915, hang);
	}

	igt_subtest("virtual-busy-hang")
		virtual(i915, &ctx->cfg, TEST_BUSY | FLAG_HANG);

	/**
	 * Test that when one engine is loaded other report no
	 * load.
	 */
	test_each_engine("busy-check-all", i915, ctx, e)
		busy_check_all(i915, ctx, e, num_engines, classes, num_classes,
			       TEST_BUSY);

	test_each_engine("busy-idle-check-all", i915, ctx, e)
		busy_check_all(i915, ctx, e, num_engines, classes, num_classes,
			       TEST_BUSY | TEST_TRAILING_IDLE);

	/**
	 * Test that when all except one engine are loaded all
	 * loads are correctly reported.
	 */
	test_each_engine("most-busy-check-all", i915, ctx, e)
		most_busy_check_all(i915, ctx, e, num_engines,
				    classes, num_classes,
				    TEST_BUSY);

	test_each_engine("most-busy-idle-check-all", i915, ctx, e)
		most_busy_check_all(i915, ctx, e, num_engines,
				    classes, num_classes,
				    TEST_BUSY | TEST_TRAILING_IDLE);

	/**
	 * Test that when all engines are loaded all loads are
	 * correctly reported.
	 */
	igt_subtest("all-busy-check-all")
		all_busy_check_all(i915, ctx, num_engines, classes, num_classes,
				   TEST_BUSY);

	igt_subtest("all-busy-idle-check-all")
		all_busy_check_all(i915, ctx, num_engines, classes, num_classes,
				   TEST_BUSY | TEST_TRAILING_IDLE);

	igt_subtest("virtual-busy-all")
		virtual_all(i915, &ctx->cfg, TEST_BUSY);

	igt_subtest("virtual-busy-idle-all")
		virtual_all(i915, &ctx->cfg, TEST_BUSY | TEST_TRAILING_IDLE);

	igt_subtest("virtual-busy-hang-all")
		virtual_all(i915, &ctx->cfg, TEST_BUSY | FLAG_HANG);
	/**
	 * Test for no cross-client contamination.
	 */
	test_each_engine("isolation", i915, ctx, e)
		single(i915, ctx, e, TEST_BUSY | TEST_ISOLATION);

	igt_subtest_with_dynamic("memory-info-idle") {
		for_each_memory_region(r, i915) {
			igt_dynamic_f("%s", r->name)
				test_memory(i915, r, 0);
		}
	}

	igt_subtest_with_dynamic("memory-info-resident") {
		for_each_memory_region(r, i915) {
			igt_dynamic_f("%s", r->name)
				test_memory(i915, r, TEST_RESIDENT);
		}
	}

	igt_subtest_with_dynamic("memory-info-purgeable") {
		for_each_memory_region(r, i915) {
			igt_dynamic_f("%s", r->name)
				test_memory(i915, r, TEST_PURGEABLE);
		}
	}

	igt_subtest_with_dynamic("memory-info-active") {
		for_each_memory_region(r, i915) {
			igt_dynamic_f("%s", r->name)
				test_memory(i915, r, TEST_ACTIVE);
		}
	}

	igt_subtest_with_dynamic("memory-info-shared") {
		for_each_memory_region(r, i915) {
			igt_dynamic_f("%s", r->name)
				test_memory(i915, r, TEST_SHARED);
		}
	}

	igt_subtest_group() {
		int newfd;

		igt_fixture()
			newfd = drm_reopen_driver(i915);

		igt_subtest("context-close-stress")
			stress_context_close(newfd);

		igt_fixture()
			drm_close_driver(newfd);
	}

	igt_fixture() {
		intel_ctx_destroy(i915, ctx);
		drm_close_driver(i915);
	}
}
