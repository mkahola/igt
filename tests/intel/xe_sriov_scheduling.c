// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */
#include "igt.h"
#include "igt_sriov_device.h"
#include "igt_syncobj.h"
#include "igt_sysfs.h"
#include "xe_drm.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_spin.h"
#include "xe/xe_sriov_provisioning.h"

/**
 * TEST: Tests for SR-IOV scheduling parameters.
 * Category: Core
 * Mega feature: SR-IOV
 * Sub-category: scheduling
 * Functionality: vGPU profiles scheduling parameters
 * Description: Verify behavior after modifying scheduling attributes.
 */

enum subm_sync_method { SYNC_NONE, SYNC_BARRIER };

struct subm_opts {
	enum subm_sync_method sync_method;
	uint32_t exec_quantum_ms;
	uint32_t preempt_timeout_us;
	double outlier_treshold;
	/* --inflight=0 => auto; >=1 => explicit K */
	unsigned int inflight;
};

struct subm_work_desc {
	uint64_t duration_ms;
	bool preempt;
	unsigned int repeats;
};

struct subm_stats {
	igt_stats_t samples;
	uint64_t start_timestamp;
	uint64_t end_timestamp;
	uint64_t *complete_ts; /* absolute completion timestamps (ns) */
	unsigned int num_early_finish;
	unsigned int concurrent_execs;
	double concurrent_rate;
	double concurrent_mean;
};

struct subm {
	char id[32];
	int fd;
	int vf_num;
	struct subm_work_desc work;
	uint32_t expected_ticks;
	uint32_t vm;
	struct drm_xe_engine_class_instance hwe;
	uint32_t exec_queue_id;
	/* K slots (K BOs / addresses / mapped spinners / done fences / submit timestamps) */
	unsigned int slots;
	uint64_t *addr;
	uint32_t *bo;
	size_t bo_size;
	struct xe_spin **spin;
	uint32_t *done_fence;
	uint64_t *submit_ts;
	struct drm_xe_sync sync[1];
	struct drm_xe_exec exec;
};

struct subm_thread_data {
	struct subm subm;
	struct subm_stats stats;
	const struct subm_opts *opts;
	pthread_t thread;
	pthread_barrier_t *barrier;
};

struct subm_set {
	struct subm_thread_data *data;
	int ndata;
	enum subm_sync_method sync_method;
	pthread_barrier_t barrier;
};

static void subm_init(struct subm *s, int fd, int vf_num, uint64_t addr,
		      struct drm_xe_engine_class_instance hwe,
		      unsigned int inflight)
{
	uint64_t base, stride;

	memset(s, 0, sizeof(*s));
	s->fd = fd;
	s->vf_num = vf_num;
	s->hwe = hwe;
	snprintf(s->id, sizeof(s->id), "VF%d %d:%d:%d", vf_num,
		 hwe.engine_class, hwe.engine_instance, hwe.gt_id);
	s->slots = inflight ? inflight : 1;
	s->vm = xe_vm_create(s->fd, 0, 0);
	s->exec_queue_id = xe_exec_queue_create(s->fd, s->vm, &s->hwe, 0);
	s->bo_size = ALIGN(sizeof(struct xe_spin) + xe_cs_prefetch_size(s->fd),
			   xe_get_default_alignment(s->fd));
	s->addr = calloc(s->slots, sizeof(*s->addr));
	s->bo = calloc(s->slots, sizeof(*s->bo));
	s->spin = calloc(s->slots, sizeof(*s->spin));
	s->done_fence = calloc(s->slots, sizeof(*s->done_fence));
	s->submit_ts = calloc(s->slots, sizeof(*s->submit_ts));

	igt_assert(s->addr && s->bo && s->spin && s->done_fence && s->submit_ts);

	base = addr ? addr : 0x1a0000;
	stride = ALIGN(s->bo_size, 0x10000);
	for (unsigned int i = 0; i < s->slots; i++) {
		s->addr[i] = base + i * stride;
		s->bo[i] = xe_bo_create(s->fd, s->vm, s->bo_size,
					vram_if_possible(fd, s->hwe.gt_id),
					DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
		s->spin[i] = xe_bo_map(s->fd, s->bo[i], s->bo_size);
		xe_vm_bind_sync(s->fd, s->vm, s->bo[i], 0, s->addr[i], s->bo_size);
		s->done_fence[i] = syncobj_create(s->fd, 0);
	}

	s->exec.num_batch_buffer = 1;
	s->exec.exec_queue_id = s->exec_queue_id;
	/* s->exec.address set per submission */
}

static void subm_fini(struct subm *s)
{
	for (unsigned int i = 0; i < s->slots; i++) {
		xe_vm_unbind_sync(s->fd, s->vm, 0, s->addr[i], s->bo_size);
		gem_munmap(s->spin[i], s->bo_size);
		gem_close(s->fd, s->bo[i]);
		if (s->done_fence[i])
			syncobj_destroy(s->fd, s->done_fence[i]);
	}
	xe_exec_queue_destroy(s->fd, s->exec_queue_id);
	xe_vm_destroy(s->fd, s->vm);
	free(s->addr);
	free(s->bo);
	free(s->spin);
	free(s->done_fence);
	free(s->submit_ts);
}

static void subm_workload_init(struct subm *s, struct subm_work_desc *work)
{
	s->work = *work;
	s->expected_ticks = xe_spin_nsec_to_ticks(s->fd, s->hwe.gt_id,
						  s->work.duration_ms * 1000000);
	for (unsigned int i = 0; i < s->slots; i++)
		xe_spin_init_opts(s->spin[i], .addr = s->addr[i],
				  .preempt = s->work.preempt,
				  .ctx_ticks = s->expected_ticks);
}

static void subm_wait_slot(struct subm *s, unsigned int slot, uint64_t abs_timeout_nsec)
{
	igt_assert(syncobj_wait(s->fd, &s->done_fence[slot], 1,
				abs_timeout_nsec, 0, NULL));
}

static void subm_exec_slot(struct subm *s, unsigned int slot)
{
	struct timespec tv;

	syncobj_reset(s->fd, &s->done_fence[slot], 1);
	memset(&s->sync[0], 0, sizeof(s->sync));
	s->sync[0].type = DRM_XE_SYNC_TYPE_SYNCOBJ;
	s->sync[0].flags = DRM_XE_SYNC_FLAG_SIGNAL;
	s->sync[0].handle = s->done_fence[slot];
	s->exec.num_syncs = 1;
	s->exec.syncs = to_user_pointer(&s->sync[0]);
	s->exec.address = s->addr[slot];
	igt_gettime(&tv);
	s->submit_ts[slot] = (uint64_t)tv.tv_sec * (uint64_t)NSEC_PER_SEC + (uint64_t)tv.tv_nsec;
	xe_exec(s->fd, &s->exec);
}

static bool subm_is_work_complete(struct subm *s, unsigned int slot)
{
	return s->expected_ticks <= ~s->spin[slot]->ticks_delta;
}

static bool subm_is_exec_queue_banned(struct subm *s)
{
	struct drm_xe_exec_queue_get_property args = {
		.exec_queue_id = s->exec_queue_id,
		.property = DRM_XE_EXEC_QUEUE_GET_PROPERTY_BAN,
	};
	int ret = igt_ioctl(s->fd, DRM_IOCTL_XE_EXEC_QUEUE_GET_PROPERTY, &args);

	return ret || args.value;
}

static void subm_exec_loop(struct subm *s, struct subm_stats *stats,
			   const struct subm_opts *opts)
{
	const unsigned int inflight = s->slots;
	unsigned int submitted = 0;
	struct timespec tv;
	unsigned int i;

	igt_gettime(&tv);
	stats->start_timestamp =
		tv.tv_sec * (uint64_t)NSEC_PER_SEC + tv.tv_nsec;
	igt_debug("[%s] start_timestamp: %f\n", s->id, stats->start_timestamp * 1e-9);

	/* Prefill */
	if (s->work.repeats) {
		unsigned int can_prefill = min(inflight, s->work.repeats);

		for (i = 0; i < can_prefill; i++)
			subm_exec_slot(s, i % inflight);
		submitted = can_prefill;
	}

	/* Process completions in order: sample i -> slot (i % inflight) */
	for (i = 0; i < s->work.repeats; ++i) {
		unsigned int slot = i % inflight;

		subm_wait_slot(s, slot, INT64_MAX);
		igt_gettime(&tv);
		stats->complete_ts[i] = (uint64_t)tv.tv_sec * (uint64_t)NSEC_PER_SEC +
					(uint64_t)tv.tv_nsec;
		igt_stats_push(&stats->samples, stats->complete_ts[i] - s->submit_ts[slot]);

		if (!subm_is_work_complete(s, slot)) {
			stats->num_early_finish++;

			igt_debug("[%s] subm #%d early_finish=%u\n",
				  s->id, i, stats->num_early_finish);

			if (subm_is_exec_queue_banned(s))
				break;
		}

		/* Keep the pipeline full */
		if (submitted < s->work.repeats) {
			unsigned int next_slot = submitted % inflight;

			subm_exec_slot(s, next_slot);
			submitted++;
		}
	}

	igt_gettime(&tv);
	stats->end_timestamp = tv.tv_sec * (uint64_t)NSEC_PER_SEC + tv.tv_nsec;
	igt_debug("[%s] end_timestamp: %f\n", s->id, stats->end_timestamp * 1e-9);
}

static void *subm_thread(void *thread_data)
{
	struct subm_thread_data *td = thread_data;
	struct timespec tv;

	igt_gettime(&tv);
	igt_debug("[%s] thread started %ld.%ld\n", td->subm.id, tv.tv_sec,
		  tv.tv_nsec);

	if (td->barrier)
		pthread_barrier_wait(td->barrier);

	subm_exec_loop(&td->subm, &td->stats, td->opts);

	return NULL;
}

static void subm_set_dispatch_and_wait_threads(struct subm_set *set)
{
	int i;

	for (i = 0; i < set->ndata; ++i)
		igt_assert_eq(0, pthread_create(&set->data[i].thread, NULL,
						subm_thread, &set->data[i]));

	for (i = 0; i < set->ndata; ++i)
		pthread_join(set->data[i].thread, NULL);
}

static void subm_set_alloc_data(struct subm_set *set, unsigned int ndata)
{
	igt_assert(!set->data);
	set->ndata = ndata;
	set->data = calloc(set->ndata, sizeof(struct subm_thread_data));
	igt_assert(set->data);
}

static void subm_set_free_data(struct subm_set *set)
{
	free(set->data);
	set->data = NULL;
	set->ndata = 0;
}

static void subm_set_init_sync_method(struct subm_set *set, enum subm_sync_method sm)
{
	set->sync_method = sm;
	if (set->sync_method == SYNC_BARRIER)
		pthread_barrier_init(&set->barrier, NULL, set->ndata);
}

static void subm_set_close_handles(struct subm_set *set)
{
	struct subm *s;
	int i;

	if (!set->ndata)
		return;

	for (i = 0; i < set->ndata; ++i) {
		s = &set->data[i].subm;

		if (s->fd != -1) {
			subm_fini(s);
			drm_close_driver(s->fd);
			s->fd = -1;
		}
	}
}

static void subm_set_fini(struct subm_set *set)
{
	int i;

	if (!set->ndata)
		return;

	if (set->sync_method == SYNC_BARRIER)
		pthread_barrier_destroy(&set->barrier);

	subm_set_close_handles(set);

	for (i = 0; i < set->ndata; ++i) {
		igt_stats_fini(&set->data[i].stats.samples);
		free(set->data[i].stats.complete_ts);
	}

	subm_set_free_data(set);
}

struct init_vf_ids_opts {
	bool shuffle;
	bool shuffle_pf;
};

static void init_vf_ids(uint8_t *array, size_t n,
			const struct init_vf_ids_opts *opts)
{
	size_t i, j;

	if (!opts->shuffle_pf && n) {
		array[0] = 0;
		n -= 1;
		array = array + 1;
	}

	for (i = 0; i < n; i++) {
		j = (opts->shuffle) ? rand() % (i + 1) : i;

		if (j != i)
			array[i] = array[j];

		array[j] = i + (opts->shuffle_pf ? 0 : 1);
	}
}

struct vf_sched_params {
	uint32_t exec_quantum_ms;
	uint32_t preempt_timeout_us;
};

static void set_vfs_scheduling_params(int pf_fd, int num_vfs,
				      const struct vf_sched_params *p)
{
	unsigned int gt;

	xe_for_each_gt(pf_fd, gt) {
		for (int vf = 0; vf <= num_vfs; ++vf) {
			xe_sriov_set_exec_quantum_ms(pf_fd, vf, gt, p->exec_quantum_ms);
			xe_sriov_set_preempt_timeout_us(pf_fd, vf, gt, p->preempt_timeout_us);
		}
	}
}

static bool check_within_epsilon(const double x, const double ref, const double tol)
{
	return x <= (1.0 + tol) * ref && x >= (1.0 - tol) * ref;
}

static void compute_common_time_frame_stats(struct subm_set *set)
{
	struct subm_thread_data *data = set->data;
	int i, j, ndata = set->ndata;
	struct subm_stats *stats;
	uint64_t common_start = 0;
	uint64_t common_end = UINT64_MAX;
	uint64_t first_ts, last_ts;

	/* Find common window from completion timestamps */
	for (i = 0; i < ndata; i++) {
		stats = &data[i].stats;

		if (!stats->samples.n_values)
			continue;

		first_ts = stats->complete_ts[0];
		last_ts = stats->complete_ts[stats->samples.n_values - 1];

		if (first_ts > common_start)
			common_start = first_ts;
		if (last_ts < common_end)
			common_end = last_ts;
	}

	igt_info("common time frame: [%" PRIu64 ";%" PRIu64 "] %.2fms\n",
		 common_start, common_end, (common_end - common_start) / 1e6);

	if (igt_warn_on_f(common_end <= common_start, "No common time frame for all sets found\n"))
		return;

	/* Compute concurrent_rate for each sample set within the common time frame */
	for (i = 0; i < ndata; i++) {
		const double window_s = (common_end - common_start) * 1e-9;

		stats = &data[i].stats;
		stats->concurrent_execs = 0;
		stats->concurrent_rate = 0.0;
		stats->concurrent_mean = 0.0;

		for (j = 0; j < stats->samples.n_values; j++) {
			uint64_t cts = stats->complete_ts[j];

			if (cts >= common_start && cts <= common_end) {
				stats->concurrent_execs++;
				stats->concurrent_mean += stats->samples.values_u64[j];
			}
		}

		stats->concurrent_rate = (window_s > 0.0) ?
					 ((double)stats->concurrent_execs / window_s) : 0.0;
		stats->concurrent_mean = stats->concurrent_execs ?
					 (double)stats->concurrent_mean /
					 stats->concurrent_execs : 0.0;
		igt_info("[%s] Throughput = %.4f execs/s mean submit->signal latency=%.4fms nsamples=%d\n",
			 data[i].subm.id, stats->concurrent_rate, stats->concurrent_mean * 1e-6,
			 stats->concurrent_execs);
	}
}

static void log_sample_values(char *id, struct subm_stats *stats,
			      double comparison_mean, double outlier_treshold)
{
	const uint64_t *values = stats->samples.values_u64;
	unsigned int n = stats->samples.n_values;
	char buffer[2048];
	char *p = buffer, *pend = buffer + sizeof(buffer);
	unsigned int i;
	const unsigned int edge_items = 3;
	bool is_outlier;
	double tolerance = outlier_treshold * comparison_mean;

	p += snprintf(p, pend - p,
		      "[%s] start=%f end=%f nsamples=%u comparison_mean=%.2fms\n",
		      id, stats->start_timestamp * 1e-9, stats->end_timestamp * 1e-9, n,
		      comparison_mean * 1e-6);

	for (i = 0; i < n && p < pend; ++i) {
		is_outlier = fabs(values[i] - comparison_mean) > tolerance;

		if (n <= 2 * edge_items || i < edge_items ||
		    i >= n - edge_items || is_outlier) {
			if (is_outlier) {
				double pct_diff =
					100 *
					(comparison_mean ?
						 (values[i] - comparison_mean) /
							 comparison_mean :
						 1.0);

				p += snprintf(p, pend - p,
					      "%0.2f @%d Pct Diff %0.2f%%\n",
					      values[i] * 1e-6, i,
					      pct_diff);
			} else {
				p += snprintf(p, pend - p, "%0.2f\n",
					      values[i] * 1e-6);
			}
		}

		if (i == edge_items && n > 2 * edge_items)
			p += snprintf(p, pend - p, "...\n");
	}

	igt_debug("%s\n", buffer);
}

#define MIN_NUM_REPEATS 25
#define MIN_EXEC_QUANTUM_MS 1
#define MAX_EXEC_QUANTUM_MS 32
#define MIN_JOB_DURATION_MS 2
#define MAX_TOTAL_DURATION_MS 15000
#define PREFERRED_TOTAL_DURATION_MS 10000
#define MAX_PREFERRED_REPEATS 100

struct job_sched_params {
	int duration_ms;
	int num_repeats;
	struct vf_sched_params sched_params;
};

static uint32_t sysfs_get_job_timeout_ms(int fd, struct drm_xe_engine_class_instance *eci)
{
	int engine_dir;
	uint32_t ret;

	engine_dir = xe_sysfs_engine_open(fd, eci->gt_id, eci->engine_class);
	ret = igt_sysfs_get_u32(engine_dir, "job_timeout_ms");
	close(engine_dir);

	return ret;
}

static uint32_t derive_preempt_timeout_us(const uint32_t exec_quantum_ms)
{
	return exec_quantum_ms * 2 * USEC_PER_MSEC;
}

static int calculate_job_duration_ms(int execution_ms)
{
	return execution_ms * 2 > MIN_JOB_DURATION_MS ? execution_ms * 2 :
							MIN_JOB_DURATION_MS;
}

static bool compute_max_exec_quantum_ms(uint32_t *exec_quantum_ms,
					int num_threads,
					int min_num_repeats,
					int job_timeout_ms)
{
	for (int test_execution_ms = MAX_EXEC_QUANTUM_MS;
	     test_execution_ms >= MIN_EXEC_QUANTUM_MS; test_execution_ms--) {
		int test_duration_ms =
			calculate_job_duration_ms(test_execution_ms);
		int max_delay_ms = (num_threads - 1) * test_execution_ms;

		/*
		 * Check if the job can complete within job_timeout_ms,
		 * including the maximum scheduling delay
		 */
		if (test_duration_ms + max_delay_ms <= job_timeout_ms) {
			int estimated_num_repeats =
				MAX_TOTAL_DURATION_MS /
				(num_threads * test_duration_ms);

			if (estimated_num_repeats >= min_num_repeats) {
				*exec_quantum_ms = test_execution_ms;
				return true;
			}
		}
	}
	return false;
}

static int adjust_num_repeats(int duration_ms, int num_threads)
{
	int preferred_max_repeats = PREFERRED_TOTAL_DURATION_MS /
				    (num_threads * duration_ms);
	int optimal_repeats = min(preferred_max_repeats, MAX_PREFERRED_REPEATS);

	return max(optimal_repeats, MIN_NUM_REPEATS);
}

/* inflight K selection:
 *   user_k == 0  => auto
 *   user_k >= 1  => explicit K
 */
static unsigned int select_inflight_k(unsigned int duration_ms,
				      unsigned int user_k,
				      bool nonpreempt)
{
	if (user_k)
		return user_k >= 1 ? user_k : 1;
	if (nonpreempt)
		return 1;
	if (duration_ms <= 12)
		return 4;
	if (duration_ms <= 20)
		return 3;
	return 2;
}

static struct vf_sched_params prepare_vf_sched_params(int num_threads,
						      int min_num_repeats,
						      int job_timeout_ms,
						      const struct subm_opts *opts)
{
	struct vf_sched_params params = { MIN_EXEC_QUANTUM_MS,
					  derive_preempt_timeout_us(MIN_EXEC_QUANTUM_MS) };

	if (opts->exec_quantum_ms || opts->preempt_timeout_us) {
		if (opts->exec_quantum_ms)
			params.exec_quantum_ms = opts->exec_quantum_ms;
		if (opts->preempt_timeout_us)
			params.preempt_timeout_us = opts->preempt_timeout_us;
	} else {
		if (igt_debug_on(!compute_max_exec_quantum_ms(&params.exec_quantum_ms,
							      num_threads,
							      min_num_repeats,
							      job_timeout_ms)))
			return params;

		/*
		 * After computing a feasible max_exec_quantum_ms,
		 * select a random exec_quantum_ms within the new range
		 */
		params.exec_quantum_ms = MIN_EXEC_QUANTUM_MS +
					 rand() % (params.exec_quantum_ms -
						   MIN_EXEC_QUANTUM_MS + 1);
		params.preempt_timeout_us = derive_preempt_timeout_us(params.exec_quantum_ms);
	}

	return params;
}

static struct job_sched_params
prepare_job_sched_params(int num_threads, int job_timeout_ms, const struct subm_opts *opts)
{
	struct job_sched_params params = { };

	params.sched_params = prepare_vf_sched_params(num_threads, MIN_NUM_REPEATS,
						      job_timeout_ms, opts);
	params.duration_ms = calculate_job_duration_ms(params.sched_params.exec_quantum_ms);
	params.num_repeats = adjust_num_repeats(params.duration_ms, num_threads);

	return params;
}

/**
 * SUBTEST: equal-throughput
 * Description:
 *   Check all VFs with same scheduling settings running same workload
 *   achieve the same throughput.
 */
static void throughput_ratio(int pf_fd, int num_vfs, const struct subm_opts *opts)
{
	struct subm_set set_ = {}, *set = &set_;
	uint8_t vf_ids[num_vfs + 1 /*PF*/];
	uint32_t job_timeout_ms = sysfs_get_job_timeout_ms(pf_fd, &xe_engine(pf_fd, 0)->instance);
	struct job_sched_params job_sched_params = prepare_job_sched_params(num_vfs + 1,
									    job_timeout_ms,
									    opts);
	const unsigned int k = select_inflight_k(job_sched_params.duration_ms,
						 opts->inflight, false);

	igt_info("eq=%ums pt=%uus duration=%ums repeats=%d inflight=%u num_vfs=%d job_timeout=%ums\n",
		 job_sched_params.sched_params.exec_quantum_ms,
		 job_sched_params.sched_params.preempt_timeout_us,
		 job_sched_params.duration_ms, job_sched_params.num_repeats,
		 k, num_vfs + 1, job_timeout_ms);

	init_vf_ids(vf_ids, ARRAY_SIZE(vf_ids),
		    &(struct init_vf_ids_opts){ .shuffle = true,
						.shuffle_pf = true });
	xe_sriov_require_default_scheduling_attributes(pf_fd);
	/* enable VFs */
	igt_sriov_disable_driver_autoprobe(pf_fd);
	igt_sriov_enable_vfs(pf_fd, num_vfs);
	/* set scheduling params (PF and VFs) */
	set_vfs_scheduling_params(pf_fd, num_vfs, &job_sched_params.sched_params);
	/* probe VFs */
	igt_sriov_enable_driver_autoprobe(pf_fd);
	for (int vf = 1; vf <= num_vfs; ++vf)
		igt_sriov_bind_vf_drm_driver(pf_fd, vf);

	/* init subm_set */
	subm_set_alloc_data(set, num_vfs + 1 /*PF*/);
	subm_set_init_sync_method(set, opts->sync_method);

	for (int n = 0; n < set->ndata; ++n) {
		int vf_fd =
			vf_ids[n] ?
				igt_sriov_open_vf_drm_device(pf_fd, vf_ids[n]) :
				drm_reopen_driver(pf_fd);

		igt_assert_fd(vf_fd);
		set->data[n].opts = opts;
		subm_init(&set->data[n].subm, vf_fd, vf_ids[n], 0,
			  xe_engine(vf_fd, 0)->instance, k);
		subm_workload_init(&set->data[n].subm,
				   &(struct subm_work_desc){
					.duration_ms = job_sched_params.duration_ms,
					.preempt = true,
					.repeats = job_sched_params.num_repeats });
		igt_stats_init_with_size(&set->data[n].stats.samples,
					 set->data[n].subm.work.repeats);
		set->data[n].stats.complete_ts = calloc(set->data[n].subm.work.repeats,
							sizeof(uint64_t));
		if (set->sync_method == SYNC_BARRIER)
			set->data[n].barrier = &set->barrier;
	}

	/* dispatch spinners, wait for results */
	subm_set_dispatch_and_wait_threads(set);
	subm_set_close_handles(set);

	/* verify results */
	compute_common_time_frame_stats(set);
	for (int n = 0; n < set->ndata; ++n) {
		struct subm_stats *stats = &set->data[n].stats;
		const double ref_rate = set->data[0].stats.concurrent_rate;

		igt_assert_eq(0, stats->num_early_finish);
		if (!check_within_epsilon(stats->concurrent_rate, ref_rate,
					  opts->outlier_treshold)) {
			log_sample_values(set->data[0].subm.id,
					  &set->data[0].stats,
					  set->data[0].stats.concurrent_mean,
					  opts->outlier_treshold);
			log_sample_values(set->data[n].subm.id, stats,
					  set->data[0].stats.concurrent_mean,
					  opts->outlier_treshold);
			igt_assert_f(false,
				     "Throughput=%.3f execs/s not within +-%.0f%% of expected=%.3f execs/s\n",
				     stats->concurrent_rate,
				     opts->outlier_treshold * 100, ref_rate);
		}
	}

	/* cleanup */
	subm_set_fini(set);
	set_vfs_scheduling_params(pf_fd, num_vfs, &(struct vf_sched_params){});
	xe_sriov_disable_vfs_restore_auto_provisioning(pf_fd);
}

/**
 * SUBTEST: nonpreempt-engine-resets
 * Description:
 *   Check all VFs running a non-preemptible workload with a duration
 *   exceeding the sum of its execution quantum and preemption timeout,
 *   will experience engine reset due to preemption timeout.
 */
static void nonpreempt_engine_resets(int pf_fd, int num_vfs,
				     const struct subm_opts *opts)
{
	struct subm_set set_ = {}, *set = &set_;
	uint32_t job_timeout_ms = sysfs_get_job_timeout_ms(pf_fd, &xe_engine(pf_fd, 0)->instance);
	struct vf_sched_params vf_sched_params = prepare_vf_sched_params(num_vfs, 1,
									 job_timeout_ms, opts);
	uint64_t duration_ms = 2 * vf_sched_params.exec_quantum_ms +
			       vf_sched_params.preempt_timeout_us / USEC_PER_MSEC;
	int preemptible_end = 1;
	uint8_t vf_ids[num_vfs + 1 /*PF*/];
	const unsigned int k = select_inflight_k(duration_ms, opts->inflight, true);

	igt_info("eq=%ums pt=%uus duration=%" PRIu64 "ms inflight=%u num_vfs=%d job_timeout=%ums\n",
		 vf_sched_params.exec_quantum_ms, vf_sched_params.preempt_timeout_us,
		 duration_ms, k, num_vfs, job_timeout_ms);

	init_vf_ids(vf_ids, ARRAY_SIZE(vf_ids),
		    &(struct init_vf_ids_opts){ .shuffle = true,
						.shuffle_pf = true });
	xe_sriov_require_default_scheduling_attributes(pf_fd);
	/* enable VFs */
	igt_sriov_disable_driver_autoprobe(pf_fd);
	igt_sriov_enable_vfs(pf_fd, num_vfs);
	/* set scheduling params (PF and VFs) */
	set_vfs_scheduling_params(pf_fd, num_vfs, &vf_sched_params);
	/* probe VFs */
	igt_sriov_enable_driver_autoprobe(pf_fd);
	for (int vf = 1; vf <= num_vfs; ++vf)
		igt_sriov_bind_vf_drm_driver(pf_fd, vf);

	/* init subm_set */
	subm_set_alloc_data(set, num_vfs + 1 /*PF*/);
	subm_set_init_sync_method(set, opts->sync_method);

	for (int n = 0; n < set->ndata; ++n) {
		int vf_fd =
			vf_ids[n] ?
				igt_sriov_open_vf_drm_device(pf_fd, vf_ids[n]) :
				drm_reopen_driver(pf_fd);

		igt_assert_fd(vf_fd);
		set->data[n].opts = opts;
		subm_init(&set->data[n].subm, vf_fd, vf_ids[n], 0,
			  xe_engine(vf_fd, 0)->instance, k);
		subm_workload_init(&set->data[n].subm,
				   &(struct subm_work_desc){
					.duration_ms = duration_ms,
					.preempt = (n < preemptible_end),
					.repeats = MIN_NUM_REPEATS });
		igt_stats_init_with_size(&set->data[n].stats.samples,
					 set->data[n].subm.work.repeats);
		set->data[n].stats.complete_ts = calloc(set->data[n].subm.work.repeats,
							sizeof(uint64_t));
		if (set->sync_method == SYNC_BARRIER)
			set->data[n].barrier = &set->barrier;
	}

	/* dispatch spinners, wait for results */
	subm_set_dispatch_and_wait_threads(set);
	subm_set_close_handles(set);

	/* verify results */
	for (int n = 0; n < set->ndata; ++n) {
		if (n < preemptible_end) {
			igt_assert_eq(0, set->data[n].stats.num_early_finish);
			igt_assert_eq(set->data[n].subm.work.repeats,
				      set->data[n].stats.samples.n_values);
		} else {
			igt_assert_eq(1, set->data[n].stats.num_early_finish);
		}
	}

	/* cleanup */
	subm_set_fini(set);
	set_vfs_scheduling_params(pf_fd, num_vfs, &(struct vf_sched_params){});
	xe_sriov_disable_vfs_restore_auto_provisioning(pf_fd);
}

static struct subm_opts subm_opts = {
	.sync_method = SYNC_BARRIER,
	.outlier_treshold = 0.1,
	.inflight = 0,
};

static bool extended_scope;

static int subm_opts_handler(int opt, int opt_index, void *data)
{
	switch (opt) {
	case 'e':
		extended_scope = true;
		break;
	case 's':
		subm_opts.sync_method = atoi(optarg);
		igt_info("Sync method: %d\n", subm_opts.sync_method);
		break;
	case 'q':
		subm_opts.exec_quantum_ms = atoi(optarg);
		igt_info("Execution quantum ms: %u\n", subm_opts.exec_quantum_ms);
		break;
	case 'p':
		subm_opts.preempt_timeout_us = atoi(optarg);
		igt_info("Preempt timeout us: %u\n", subm_opts.preempt_timeout_us);
		break;
	case 't':
		subm_opts.outlier_treshold = atoi(optarg) / 100.0;
		igt_info("Outlier threshold: %.2f\n", subm_opts.outlier_treshold);
		break;
	case 'i': {
		int val = atoi(optarg);

		subm_opts.inflight = val > 0 ? val : 0;
		if (subm_opts.inflight)
			igt_info("In-flight submissions: %u\n", subm_opts.inflight);
		else
			igt_info("In-flight submissions: auto (0)\n");
		break;
	}
	default:
		return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

static const struct option long_opts[] = {
	{ .name = "extended", .has_arg = false, .val = 'e', },
	{ .name = "sync", .has_arg = true, .val = 's', },
	{ .name = "threshold", .has_arg = true, .val = 't', },
	{ .name = "eq_ms", .has_arg = true, .val = 'q', },
	{ .name = "pt_us", .has_arg = true, .val = 'p', },
	{ .name = "inflight", .has_arg = true, .val = 'i', },
	{}
};

static const char help_str[] =
	"  --extended\tRun the extended test scope\n"
	"  --sync\tThreads synchronization method: 0 - none 1 - barrier (Default 1)\n"
	"  --threshold\tSample outlier threshold (Default 0.1)\n"
	"  --eq_ms\texec_quantum_ms\n"
	"  --pt_us\tpreempt_timeout_us\n"
	"  --inflight\tNumber of submissions kept in flight per VF (0=auto)\n";

int igt_main_args("", long_opts, help_str, subm_opts_handler, NULL)
{
	int pf_fd;
	bool autoprobe;

	igt_fixture() {
		pf_fd = drm_open_driver(DRIVER_XE);
		igt_require(igt_sriov_is_pf(pf_fd));
		igt_require(igt_sriov_get_enabled_vfs(pf_fd) == 0);
		autoprobe = igt_sriov_is_driver_autoprobe_enabled(pf_fd);
		xe_sriov_require_default_scheduling_attributes(pf_fd);
	}

	igt_describe("Check VFs achieve equal throughput");
	igt_subtest_with_dynamic("equal-throughput") {
		if (extended_scope)
			for_each_sriov_num_vfs(pf_fd, vf)
				igt_dynamic_f("numvfs-%d", vf)
					throughput_ratio(pf_fd, vf, &subm_opts);

		for_random_sriov_vf(pf_fd, vf)
			igt_dynamic("numvfs-random")
				throughput_ratio(pf_fd, vf, &subm_opts);
	}

	igt_describe("Check VFs experience engine reset due to preemption timeout");
	igt_subtest_with_dynamic("nonpreempt-engine-resets") {
		if (extended_scope)
			for_each_sriov_num_vfs(pf_fd, vf)
				igt_dynamic_f("numvfs-%d", vf)
					nonpreempt_engine_resets(pf_fd, vf,
								 &subm_opts);

		for_random_sriov_vf(pf_fd, vf)
			igt_dynamic("numvfs-random")
				nonpreempt_engine_resets(pf_fd, vf, &subm_opts);
	}

	igt_fixture() {
		set_vfs_scheduling_params(pf_fd, igt_sriov_get_total_vfs(pf_fd),
					  &(struct vf_sched_params){});
		xe_sriov_disable_vfs_restore_auto_provisioning(pf_fd);
		/* abort to avoid execution of next tests with enabled VFs */
		igt_abort_on_f(igt_sriov_get_enabled_vfs(pf_fd) > 0,
			       "Failed to disable VF(s)");
		autoprobe ? igt_sriov_enable_driver_autoprobe(pf_fd) :
			    igt_sriov_disable_driver_autoprobe(pf_fd);
		igt_abort_on_f(autoprobe != igt_sriov_is_driver_autoprobe_enabled(pf_fd),
			       "Failed to restore sriov_drivers_autoprobe value\n");
		drm_close_driver(pf_fd);
	}
}
