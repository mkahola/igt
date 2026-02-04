// SPDX-License-Identifier: MIT
/*
 * Copyright(c) 2024 Intel Corporation. All rights reserved.
 */

#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sys/stat.h>
#include "drmtest.h"
#include "igt_core.h"
#include "igt_device.h"
#include "igt_kmod.h"
#include "igt_pci.h"
#include "igt_sriov_device.h"
#include "intel_chipset.h"
#include "intel_vram.h"
#include "linux_scaffold.h"
#include "xe/xe_mmio.h"
#include "xe/xe_query.h"
#include "xe/xe_sriov_provisioning.h"
#include "xe/xe_sriov_debugfs.h"

/**
 * TEST: xe_sriov_flr
 * Category: Core
 * Mega feature: SR-IOV
 * Sub-category: SR-IOV Reset tests
 * Functionality: FLR
 * Description: Examine behavior of SR-IOV VF FLR
 *
 * SUBTEST: flr-vf1-clear
 * Run type: BAT
 * Description:
 *   Verifies that LMEM, GGTT, and SCRATCH_REGS are properly cleared
 *   on VF1 following a Function Level Reset (FLR).
 *
 * SUBTEST: flr-each-isolation
 * Run type: FULL
 * Description:
 *   Sequentially performs FLR on each VF to verify isolation and
 *   clearing of LMEM, GGTT, and SCRATCH_REGS on the reset VF only.
 *
 * SUBTEST: flr-vfs-parallel
 * Run type: FULL
 * Description:
 *   Executes FLR on all VFs simultaneously to validate correct behavior during parallel resets.
 *
 * SUBTEST: flr-twice
 * Run type: FULL
 * Description:
 *   Initiates FLR twice in parallel on the same VF to validate behavior
 *   when multiple resets occur within a short time frame, as seen in some
 *   real-world scenarios (e.g., when starting a QEMU VM with a passed VF).
 */

IGT_TEST_DESCRIPTION("Xe tests for SR-IOV VF FLR (Functional Level Reset)");

static const char STOP_REASON_ABORT[] = "ABORT";
static const char STOP_REASON_FAIL[]  = "FAIL";
static const char STOP_REASON_SKIP[]  = "SKIP";

#define DRIVER_OVERRIDE_TIMEOUT_MS 200

static int g_wait_flr_ms = 200;

static struct g_mmio {
	struct xe_mmio *mmio;
	unsigned int num_vfs;
} g_mmio;

static inline struct xe_mmio *xe_mmio_for_vf(unsigned int vf)
{
	igt_assert_f(g_mmio.mmio, "MMIO not initialized\n");
	igt_assert_f(vf <= g_mmio.num_vfs, "VF%u out of range (<= %u)\n", vf,
		     g_mmio.num_vfs);
	return &g_mmio.mmio[vf];
}

static void init_mmio(int pf_fd, unsigned int num_vfs)
{
	igt_assert_f(!g_mmio.mmio, "MMIO already initialized\n");
	g_mmio.mmio = calloc(num_vfs + 1, sizeof(*g_mmio.mmio));
	igt_assert(g_mmio.mmio);

	for (unsigned int i = 0; i <= num_vfs; ++i)
		xe_mmio_vf_access_init(pf_fd, i, &g_mmio.mmio[i]);

	g_mmio.num_vfs = num_vfs;
}

static void cleanup_mmio(void)
{
	if (!g_mmio.mmio)
		return;

	for (size_t i = 0; i <= g_mmio.num_vfs; ++i)
		if (xe_mmio_is_initialized(&g_mmio.mmio[i]))
			xe_mmio_access_fini(&g_mmio.mmio[i]);
	free(g_mmio.mmio);
	g_mmio.mmio = NULL;
	g_mmio.num_vfs = 0;
}

/**
 * struct subcheck_data - Base structure for subcheck data.
 *
 * This structure serves as a foundational data model for various subchecks. It is designed
 * to be extended by more specific subcheck structures as needed. The structure includes
 * essential information about the subcheck environment and conditions, which are used
 * across different testing operations.
 *
 * @pf_fd: File descriptor for the Physical Function.
 * @num_vfs: Number of Virtual Functions (VFs) enabled and under test. This count is
 *           used to iterate over and manage the VFs during the testing process.
 * @tile: Tile under test.
 * @stop_reason: Pointer to a string that indicates why a subcheck should skip or fail.
 *               This field is crucial for controlling the flow of subcheck execution.
 *               If set, it should prevent further execution of the current subcheck,
 *               allowing subcheck operations to check this field and return early if
 *               a skip or failure condition is indicated. This mechanism ensures
 *               that while one subcheck may stop due to a failure or a skip condition,
 *               other subchecks can continue execution.
 *
 * Example usage:
 * A typical use of this structure involves initializing it with the necessary test setup
 * parameters, checking the `stop_reason` field before proceeding with each subcheck operation,
 * and using `pf_fd`, `num_vfs`, and `tile` as needed based on the specific subcheck requirements.
 */
struct subcheck_data {
	int pf_fd;
	int num_vfs;
	uint8_t tile;
	char *stop_reason;
};

/**
 * struct subcheck - Defines operations for managing a subcheck scenario.
 *
 * This structure holds function pointers for the key operations required
 * to manage the lifecycle of a subcheck scenario. It is used by the `verify_flr`
 * function, which acts as a template method, to call these operations in a
 * specific sequence.
 *
 * @data: Shared data necessary for all operations in the subcheck.
 *
 * @name: Name of the subcheck operation, used for identification and reporting.
 *
 * @init: Initialize the subcheck environment.
 *   Sets up the initial state required for the subcheck, including preparing
 *   resources and ensuring the system is ready for testing.
 *   @param data: Shared data needed for initialization.
 *
 * @prepare_vf: Prepare subcheck data for a specific VF.
 *   Called for each VF before FLR is performed. It might involve marking
 *   specific memory regions or setting up PTE addresses.
 *   @param vf_id: Identifier of the VF being prepared.
 *   @param data: Shared common data.
 *
 * @verify_vf: Verify the state of a VF after FLR.
 *   Checks the VF's state post FLR to ensure the expected results,
 *   such as verifying that only the FLRed VF has its state reset.
 *   @param vf_id: Identifier of the VF to verify.
 *   @param flr_vf_id: Identifier of the VF that underwent FLR.
 *   @param data: Shared common data.
 *
 * @cleanup: Clean up the subcheck environment.
 *   Releases resources and restores the system to its original state
 *   after the subchecks, ensuring no resource leaks and preparing the system
 *   for subsequent tests.
 *   @param data: Shared common data.
 */
struct subcheck {
	struct subcheck_data *data;
	const char *name;
	void (*init)(struct subcheck_data *data);
	void (*prepare_vf)(int vf_id, struct subcheck_data *data);
	void (*verify_vf)(int vf_id, int flr_vf_id, struct subcheck_data *data);
	void (*cleanup)(struct subcheck_data *data);
};

__attribute__((format(printf, 3, 0)))
static void set_stop_reason_v(struct subcheck_data *data, const char *prefix,
			      const char *format, va_list args)
{
	char *formatted_message;
	int result;

	if (igt_warn_on_f(data->stop_reason, "Stop reason already set\n"))
		return;

	result = vasprintf(&formatted_message, format, args);
	igt_assert_neq(result, -1);

	result = asprintf(&data->stop_reason, "%s : %s", prefix,
			  formatted_message);
	igt_assert_neq(result, -1);

	free(formatted_message);
}

__attribute__((format(printf, 2, 3)))
static void set_skip_reason(struct subcheck_data *data, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	set_stop_reason_v(data, STOP_REASON_SKIP, format, args);
	va_end(args);
}

__attribute__((format(printf, 2, 3)))
static void set_fail_reason(struct subcheck_data *data, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	set_stop_reason_v(data, STOP_REASON_FAIL, format, args);
	va_end(args);
}

__attribute__((format(printf, 2, 3)))
static void set_abort_reason(struct subcheck_data *data, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	set_stop_reason_v(data, STOP_REASON_ABORT, format, args);
	va_end(args);
}

static bool subcheck_can_proceed(const struct subcheck *check)
{
	return !check->data->stop_reason;
}

static int count_subchecks_with_stop_reason(struct subcheck *checks, int num_checks)
{
	int subchecks_with_stop_reason = 0;

	for (int i = 0; i < num_checks; ++i)
		if (!subcheck_can_proceed(&checks[i]))
			subchecks_with_stop_reason++;

	return subchecks_with_stop_reason;
}

static bool no_subchecks_can_proceed(struct subcheck *checks, int num_checks)
{
	return count_subchecks_with_stop_reason(checks, num_checks) == num_checks;
}

static bool is_subcheck_skipped(struct subcheck *subcheck)
{
	return subcheck->data && subcheck->data->stop_reason &&
	       !strncmp(STOP_REASON_SKIP, subcheck->data->stop_reason, strlen(STOP_REASON_SKIP));
}

static void subchecks_report_results(struct subcheck *checks, int num_checks)
{
	int fails = 0, skips = 0;

	for (int i = 0; i < num_checks; ++i) {
		if (checks[i].data->stop_reason) {
			if (is_subcheck_skipped(&checks[i])) {
				igt_info("%s: Tile%u: %s\n", checks[i].name,
					 checks[i].data->tile,
					 checks[i].data->stop_reason);
				skips++;
			} else {
				igt_critical("%s: Tile%u: %s\n", checks[i].name,
					     checks[i].data->tile,
					     checks[i].data->stop_reason);
				fails++;
			}
		} else {
			igt_info("%s: Tile%u: SUCCESS\n", checks[i].name,
				 checks[i].data->tile);
		}
	}

	igt_fail_on_f(fails, "%d out of %d checks failed\n", fails, num_checks);
	igt_skip_on(skips == num_checks);
}

static bool vf_bind_driver_override(int pf_fd, unsigned int vf_id,
				    const char *driver)
{
	char *slot = igt_sriov_get_vf_pci_slot_alloc(pf_fd, vf_id);
	int ret;
	char bound[64] = "none";
	int bound_ret;

	igt_assert(slot);
	igt_assert(driver);

	ret = igt_pci_bind_driver_override(slot, driver, DRIVER_OVERRIDE_TIMEOUT_MS);
	if (ret < 0) {
		bound_ret = igt_pci_get_bound_driver_name(slot, bound, sizeof(bound));
		if (bound_ret <= 0)
			snprintf(bound, sizeof(bound), "%s", "none");

		igt_warn_on_f(true,
			      "bind %s (VF%u) to %s ret=%d (currently bound: %s)\n",
			      slot, vf_id, driver, ret, bound);
	}

	free(slot);

	return ret >= 0;
}

static void vf_unbind_driver_override(int pf_fd, unsigned int vf_id)
{
	char *slot = igt_sriov_get_vf_pci_slot_alloc(pf_fd, vf_id);
	int ret;

	igt_assert(slot);

	ret = igt_pci_unbind_driver_override(slot, DRIVER_OVERRIDE_TIMEOUT_MS);
	igt_warn_on_f(ret < 0, "unbind %s (VF%u) driver_override ret=%d\n",
		      slot, vf_id, ret);

	free(slot);
}

/**
 * flr_exec_strategy - Function pointer for FLR execution strategy
 * @pf_fd: File descriptor for the Physical Function (PF).
 * @num_vfs: Total number of Virtual Functions (VFs) to test.
 * @checks: Array of subchecks.
 * @num_checks: Number of subchecks.
 * @wait_flr_ms: Time to wait (in milliseconds) for FLR to complete
 *
 * Defines a strategy for executing FLRs (Functional Level Resets)
 * across multiple VFs. The strategy determines the order and
 * manner (e.g., sequential or parallel) in which FLRs are performed.
 * It is expected to initiate FLRs and handle related operations,
 * such as verifying and preparing subchecks.
 *
 * Return: The ID of the last VF for which FLR was successfully initiated.
 */
typedef int (*flr_exec_strategy)(int pf_fd, int num_vfs,
				 struct subcheck *checks, int num_checks,
				 const int wait_flr_ms);

/**
 * verify_flr - Orchestrates the verification of Function Level Reset (FLR)
 *              across multiple Virtual Functions (VFs).
 *
 * This function performs FLR on each VF to ensure that only the reset VF has
 * its state cleared, while other VFs remain unaffected. It handles initialization,
 * preparation, verification, and cleanup for each test operation defined in `checks`.
 *
 * @pf_fd: File descriptor for the Physical Function (PF).
 * @num_vfs: Total number of Virtual Functions (VFs) to test.
 * @checks: Array of subchecks.
 * @num_checks: Number of subchecks.
 * @flr_exec_strategy: Execution strategy for FLR (e.g., sequential or parallel).
 *
 * Detailed Workflow:
 * - Initializes and prepares VFs for testing.
 * - Executes the FLR operation using the provided execution strategy
 *   (e.g., sequential or parallel) and validates that the reset VF behaves
 *   as expected.
 * - Cleans up resources and reports results after all VFs have been tested
 *   or in the case of an early exit.
 *
 * A timeout is used to wait for FLR operations to complete.
 */
static void verify_flr(int pf_fd, int num_vfs, struct subcheck *checks,
		       int num_checks, flr_exec_strategy exec_strategy)
{
	const int wait_flr_ms = g_wait_flr_ms;
	int i, vf_id, flr_vf_id = -1;
	bool xe_vfio_loaded;
	bool *vf_bound = NULL;

	igt_sriov_disable_driver_autoprobe(pf_fd);
	igt_sriov_enable_vfs(pf_fd, num_vfs);
	if (igt_warn_on(!igt_sriov_device_reset_exists(pf_fd, 1)))
		goto disable_vfs;

	/* Refresh PCI state */
	if (igt_warn_on(igt_pci_system_reinit()))
		goto disable_vfs;

	xe_vfio_loaded = igt_kmod_load("xe_vfio_pci", NULL) >= 0;
	if (xe_vfio_loaded) {
		vf_bound = calloc(num_vfs + 1, sizeof(*vf_bound));
		igt_assert(vf_bound);

		igt_sriov_enable_driver_autoprobe(pf_fd);
		for (vf_id = 1; vf_id <= num_vfs; vf_id++)
			vf_bound[vf_id] = vf_bind_driver_override(pf_fd, vf_id, "xe-vfio-pci");
	}

	init_mmio(pf_fd, num_vfs);

	for (i = 0; i < num_checks; ++i)
		checks[i].init(checks[i].data);

	for (vf_id = 1; vf_id <= num_vfs; ++vf_id)
		for (i = 0; i < num_checks; ++i)
			if (subcheck_can_proceed(&checks[i]))
				checks[i].prepare_vf(vf_id, checks[i].data);

	if (no_subchecks_can_proceed(checks, num_checks))
		goto cleanup;

	/* Execute the chosen FLR strategy */
	flr_vf_id = exec_strategy(pf_fd, num_vfs, checks, num_checks, wait_flr_ms);

cleanup:
	for (i = 0; i < num_checks; ++i)
		checks[i].cleanup(checks[i].data);

	cleanup_mmio();

	if (xe_vfio_loaded) {
		for (vf_id = 1; vf_id <= num_vfs; vf_id++)
			if (vf_bound && vf_bound[vf_id])
				vf_unbind_driver_override(pf_fd, vf_id);
	}

	free(vf_bound);

disable_vfs:
	igt_sriov_disable_vfs(pf_fd);

	if (flr_vf_id > 0 || no_subchecks_can_proceed(checks, num_checks))
		subchecks_report_results(checks, num_checks);
	else
		igt_skip("No checks executed\n");
}

static int execute_sequential_flr(int pf_fd, int num_vfs,
				  struct subcheck *checks, int num_checks,
				  const int wait_flr_ms)
{
	int i, vf_id, flr_vf_id = 1;

	do {
		if (igt_warn_on_f(!igt_sriov_device_reset(pf_fd, flr_vf_id),
				  "Initiating VF%u FLR failed\n", flr_vf_id))
			break;

		/* Assume FLR is finished after wait_flr_ms */
		usleep(wait_flr_ms * 1000);

		for (vf_id = 1; vf_id <= num_vfs; ++vf_id)
			for (i = 0; i < num_checks; ++i)
				if (subcheck_can_proceed(&checks[i]))
					checks[i].verify_vf(vf_id, flr_vf_id, checks[i].data);

		/* Reinitialize test data for the FLRed VF */
		if (flr_vf_id < num_vfs)
			for (i = 0; i < num_checks; ++i)
				if (subcheck_can_proceed(&checks[i]))
					checks[i].prepare_vf(flr_vf_id, checks[i].data);

		if (no_subchecks_can_proceed(checks, num_checks))
			break;

	} while (++flr_vf_id <= num_vfs);

	return flr_vf_id - 1;
}

pthread_mutex_t signal_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t signal_cond = PTHREAD_COND_INITIALIZER;

enum thread_signal {
	SIGNAL_WAIT,
	SIGNAL_START,
	SIGNAL_SKIP
} thread_signal = SIGNAL_WAIT;

struct flr_thread_data {
	int pf_fd;
	int vf_id;
	int flr_instance;
	int result;
};

static void *flr_thread(void *arg)
{
	struct flr_thread_data *data = (struct flr_thread_data *)arg;

	pthread_mutex_lock(&signal_mutex);
	while (thread_signal == SIGNAL_WAIT)
		pthread_cond_wait(&signal_cond, &signal_mutex);
	pthread_mutex_unlock(&signal_mutex);

	if (thread_signal == SIGNAL_START &&
	    igt_warn_on_f(!igt_sriov_device_reset(data->pf_fd, data->vf_id),
			  "Initiating VF%u FLR failed (flr_instance=%u)\n",
			  data->vf_id, data->flr_instance))
		data->result = -1;

	return NULL;
}

static int execute_parallel_flr_(int pf_fd, int num_vfs,
				 struct subcheck *checks,
				 int num_checks, const int wait_flr_ms,
				 unsigned int num_flrs_per_vf)
{
	const unsigned int total_flrs = num_vfs * num_flrs_per_vf;
	pthread_t threads[total_flrs];
	struct flr_thread_data thread_data[total_flrs];
	int vf_id = 0, last_vf_id = 0;
	int i, j, k, created_threads = 0;

	igt_assert(total_flrs > 0);

	for (i = 0; i < num_vfs; ++i) {
		for (j = 0; j < num_flrs_per_vf; ++j) {
			thread_data[created_threads].pf_fd = pf_fd;
			thread_data[created_threads].vf_id = i + 1; // VF IDs are 1-based
			thread_data[created_threads].flr_instance = j;
			thread_data[created_threads].result = 0;

			if (pthread_create(&threads[created_threads], NULL,
					   flr_thread,
					   &thread_data[created_threads])) {
				last_vf_id = i + 1;

				goto cleanup_threads;
			} else {
				created_threads++;
			}
		}
	}

cleanup_threads:
	pthread_mutex_lock(&signal_mutex);
	thread_signal = (created_threads == total_flrs) ? SIGNAL_START :
							  SIGNAL_SKIP;
	pthread_cond_broadcast(&signal_cond);
	pthread_mutex_unlock(&signal_mutex);

	for (i = 0; i < created_threads; ++i)
		pthread_join(threads[i], NULL);

	if (last_vf_id) {
		for (k = 0; k < num_checks; ++k)
			set_skip_reason(checks[k].data,
					"Thread creation failed for VF%u\n", last_vf_id);
		return 0;
	}

	/* Assume FLRs finished after wait_flr_ms */
	usleep(wait_flr_ms * 1000);

	/* Verify results */
	for (i = 0; i < created_threads; ++i) {
		vf_id = thread_data[i].vf_id;

		/* Skip already checked VF or if the FLR initiation failed */
		if (vf_id == last_vf_id || thread_data[i].result != 0)
			continue;

		for (k = 0; k < num_checks; ++k)
			if (subcheck_can_proceed(&checks[k]))
				checks[k].verify_vf(vf_id, vf_id, checks[k].data);

		if (no_subchecks_can_proceed(checks, num_checks))
			break;

		last_vf_id = vf_id;
	}

	return last_vf_id;
}

static int execute_parallel_flr(int pf_fd, int num_vfs, struct subcheck *checks,
				int num_checks, const int wait_flr_ms)
{
	return execute_parallel_flr_(pf_fd, num_vfs, checks, num_checks,
				     wait_flr_ms, 1);
}

static int execute_parallel_flr_twice(int pf_fd, int num_vfs,
				      struct subcheck *checks, int num_checks,
				      const int wait_flr_ms)
{
	return execute_parallel_flr_(pf_fd, num_vfs, checks, num_checks,
				     wait_flr_ms, 2);
}

#define GEN12_VF_CAP_REG			0x1901f8
#define GGTT_PTE_TEST_FIELD_MASK		GENMASK_ULL(19, 12)
#define GGTT_PTE_ADDR_SHIFT			12

struct ggtt_ops {
	void (*set_pte)(struct xe_mmio *mmio, uint8_t tile, uint32_t pte_offset, xe_ggtt_pte_t pte);
	xe_ggtt_pte_t (*get_pte)(struct xe_mmio *mmio, uint8_t tile, uint32_t pte_offset);
};

struct ggtt_provisioned_offset_range {
	uint32_t start;
	uint32_t end;
};

#define for_each_pte_offset(pte_offset__, ggtt_offset_range__) \
	for ((pte_offset__) = ((ggtt_offset_range__)->start);  \
	     (pte_offset__) <= ((ggtt_offset_range__)->end);   \
	     (pte_offset__) += sizeof(xe_ggtt_pte_t))

struct ggtt_data {
	struct subcheck_data base;
	struct ggtt_provisioned_offset_range *pte_offsets;
	struct ggtt_ops ggtt;
};

static xe_ggtt_pte_t intel_get_pte(struct xe_mmio *mmio, uint8_t tile, uint32_t pte_offset)
{
	return xe_mmio_ggtt_read(mmio, tile, pte_offset);
}

static void intel_set_pte(struct xe_mmio *mmio, uint8_t tile,
			  uint32_t pte_offset, xe_ggtt_pte_t pte)
{
	xe_mmio_ggtt_write(mmio, tile, pte_offset, pte);
}

static void intel_mtl_set_pte(struct xe_mmio *mmio, uint8_t tile,
			      uint32_t pte_offset, xe_ggtt_pte_t pte)
{
	xe_mmio_ggtt_write(mmio, tile, pte_offset, pte);

	/* force flush by read some MMIO register */
	xe_mmio_tile_read32(mmio, tile, GEN12_VF_CAP_REG);
}

static bool set_pte_gpa(struct ggtt_ops *ggtt, struct xe_mmio *mmio, uint8_t tile,
			uint32_t pte_offset, uint8_t gpa, xe_ggtt_pte_t *out)
{
	xe_ggtt_pte_t pte;

	pte = ggtt->get_pte(mmio, tile, pte_offset);
	pte &= ~GGTT_PTE_TEST_FIELD_MASK;
	pte |= ((xe_ggtt_pte_t)gpa << GGTT_PTE_ADDR_SHIFT) & GGTT_PTE_TEST_FIELD_MASK;
	ggtt->set_pte(mmio, tile, pte_offset, pte);
	*out = ggtt->get_pte(mmio, tile, pte_offset);

	return *out == pte;
}

static bool check_pte_gpa(struct ggtt_ops *ggtt, struct xe_mmio *mmio, uint8_t tile,
			  uint32_t pte_offset, uint8_t expected_gpa, xe_ggtt_pte_t *out)
{
	uint8_t val;

	*out = ggtt->get_pte(mmio, tile, pte_offset);
	val = (uint8_t)((*out & GGTT_PTE_TEST_FIELD_MASK) >> GGTT_PTE_ADDR_SHIFT);

	return val == expected_gpa;
}

static int populate_ggtt_pte_offsets(struct ggtt_data *gdata)
{
	int ret, pf_fd = gdata->base.pf_fd, num_vfs = gdata->base.num_vfs;
	struct xe_sriov_provisioned_range *ranges;
	uint8_t tile = gdata->base.tile;
	unsigned int nr_ranges;
	struct xe_mmio *mmio = xe_mmio_for_vf(0);

	gdata->pte_offsets = calloc(num_vfs + 1, sizeof(*gdata->pte_offsets));
	igt_assert(gdata->pte_offsets);

	ret = xe_sriov_find_ggtt_provisioned_pte_offsets(pf_fd, tile, mmio,
							 &ranges, &nr_ranges);
	if (ret) {
		set_abort_reason(&gdata->base, "Failed to scan GGTT PTE offset ranges (%d)\n",
				 ret);
		return -1;
	}

	for (unsigned int i = 0; i < nr_ranges; ++i) {
		const unsigned int vf_id = ranges[i].vf_id;

		if (vf_id == 0)
			continue;

		if (vf_id < 1 || vf_id > num_vfs) {
			set_abort_reason(&gdata->base,
					 "Unexpected VF%u at range entry %u [%#" PRIx64
					 "-%#" PRIx64 "], num_vfs=%u\n",
					 vf_id, i, ranges[i].start, ranges[i].end, num_vfs);
			free(ranges);
			return -1;
		}

		if (gdata->pte_offsets[vf_id].end) {
			set_abort_reason(&gdata->base, "Duplicate GGTT PTE offset range for VF%u\n",
					 vf_id);
			free(ranges);
			return -1;
		}

		gdata->pte_offsets[vf_id].start = ranges[i].start;
		gdata->pte_offsets[vf_id].end = ranges[i].end;
	}

	free(ranges);

	for (int vf_id = 1; vf_id <= num_vfs; ++vf_id)
		if (!gdata->pte_offsets[vf_id].end) {
			set_abort_reason(&gdata->base,
					 "Failed to find VF%u provisioned GGTT PTE offset range\n",
					 vf_id);
			return -1;
		}

	return 0;
}

static void ggtt_subcheck_init(struct subcheck_data *data)
{
	struct ggtt_data *gdata = (struct ggtt_data *)data;

	gdata->ggtt.get_pte = intel_get_pte;
	if (IS_METEORLAKE(intel_get_drm_devid(data->pf_fd)))
		gdata->ggtt.set_pte = intel_mtl_set_pte;
	else
		gdata->ggtt.set_pte = intel_set_pte;

	if (populate_ggtt_pte_offsets(gdata))
		/* skip reason set in populate_ggtt_pte_offsets */
		return;
}

static void ggtt_subcheck_prepare_vf(int vf_id, struct subcheck_data *data)
{
	struct ggtt_data *gdata = (struct ggtt_data *)data;
	struct xe_mmio *mmio = xe_mmio_for_vf(0);
	xe_ggtt_pte_t pte;
	uint32_t pte_offset;

	if (data->stop_reason)
		return;

	igt_debug("Tile%u: Prepare gpa on VF%u offset range [%#x-%#x]\n",
		  gdata->base.tile, vf_id,
		  gdata->pte_offsets[vf_id].start,
		  gdata->pte_offsets[vf_id].end);

	for_each_pte_offset(pte_offset, &gdata->pte_offsets[vf_id]) {
		if (!set_pte_gpa(&gdata->ggtt, mmio, data->tile, pte_offset,
				 (uint8_t)vf_id, &pte)) {
			set_abort_reason(data,
					 "Prepare VF%u failed, unexpected gpa: Read PTE: %#" PRIx64 " at offset: %#x\n",
					 vf_id, pte, pte_offset);
			return;
		}
	}
}

static void ggtt_subcheck_verify_vf(int vf_id, int flr_vf_id, struct subcheck_data *data)
{
	struct ggtt_data *gdata = (struct ggtt_data *)data;
	uint8_t expected = (vf_id == flr_vf_id) ? 0 : vf_id;
	struct xe_mmio *mmio = xe_mmio_for_vf(0);
	xe_ggtt_pte_t pte;
	uint32_t pte_offset;

	if (data->stop_reason)
		return;

	for_each_pte_offset(pte_offset, &gdata->pte_offsets[vf_id]) {
		if (!check_pte_gpa(&gdata->ggtt, mmio, data->tile, pte_offset,
				   expected, &pte)) {
			set_fail_reason(data,
					"GGTT check after VF%u FLR failed on VF%u: Read PTE: %#" PRIx64 " at offset: %#x\n",
					flr_vf_id, vf_id, pte, pte_offset);
			return;
		}
	}
}

static void ggtt_subcheck_cleanup(struct subcheck_data *data)
{
	struct ggtt_data *gdata = (struct ggtt_data *)data;

	free(gdata->pte_offsets);
}

struct lmem_data {
	struct subcheck_data base;
	size_t *vf_lmem_size;
};

const size_t STEP = SZ_1M;

static bool lmem_write_pattern(struct vram_mapping *m, uint8_t value, size_t start, size_t step)
{
	uint8_t read;

	for (; start < m->size; start += step) {
		read = intel_vram_write_readback8(m, start, value);
		if (igt_debug_on_f(read != value, "LMEM[%zu]=%u != %u\n", start, read, value))
			return false;
	}
	return true;
}

static bool lmem_contains_expected_values_(struct vram_mapping *m,
					   uint8_t expected, size_t start,
					   size_t step)
{
	uint8_t read;

	for (; start < m->size; start += step) {
		read = intel_vram_read8(m, start);
		if (igt_debug_on_f(read != expected,
				   "LMEM[%zu]=%u != %u\n", start, read, expected))
			return false;
	}
	return true;
}

static bool lmem_contains_expected_values(int pf_fd, int vf_num, size_t length,
					  char expected)
{
	struct vram_mapping vram;
	bool result;

	if (igt_debug_on(intel_vram_mmap(pf_fd, vf_num, 0, length, PROT_READ | PROT_WRITE, &vram)))
		return false;

	result = lmem_contains_expected_values_(&vram, expected, 0, STEP);
	intel_vram_munmap(&vram);

	return result;
}

static bool lmem_mmap_write_munmap(int pf_fd, int vf_num, size_t length, char value)
{
	struct vram_mapping vram;
	bool result;

	if (igt_debug_on(intel_vram_mmap(pf_fd, vf_num, 0, length, PROT_READ | PROT_WRITE, &vram)))
		return false;
	result = lmem_write_pattern(&vram, value, 0, STEP);
	intel_vram_munmap(&vram);

	return result;
}

static int populate_vf_lmem_sizes(struct subcheck_data *data)
{
	struct lmem_data *ldata = (struct lmem_data *)data;
	struct xe_sriov_provisioned_range *ranges;
	unsigned int nr_ranges, main_gt;
	int ret;

	main_gt = xe_tile_get_main_gt_id(data->pf_fd, data->tile);
	ldata->vf_lmem_size = calloc(data->num_vfs + 1, sizeof(size_t));
	igt_assert(ldata->vf_lmem_size);

	ret = xe_sriov_pf_debugfs_read_provisioned_ranges(data->pf_fd,
							  XE_SRIOV_SHARED_RES_LMEM,
							  main_gt, &ranges, &nr_ranges);
	if (ret) {
		set_abort_reason(data, "Failed read %s on main GT (%d)\n",
				 xe_sriov_debugfs_provisioned_attr_name(XE_SRIOV_SHARED_RES_LMEM),
				 ret);
		return -1;
	}

	for (unsigned int i = 0; i < nr_ranges; ++i) {
		const unsigned int vf_id = ranges[i].vf_id;

		igt_assert(vf_id >= 1 && vf_id <= data->num_vfs);
		/* Sum the allocation for vf_id (inclusive range) */
		ldata->vf_lmem_size[vf_id] += ranges[i].end - ranges[i].start + 1;
	}

	free(ranges);

	for (int vf_id = 1; vf_id <= data->num_vfs; ++vf_id)
		if (!ldata->vf_lmem_size[vf_id]) {
			set_abort_reason(data, "No LMEM provisioned for VF%u\n", vf_id);
			return -1;
		}

	return 0;
}

static void lmem_subcheck_init(struct subcheck_data *data)
{
	igt_assert_fd(data->pf_fd);
	igt_assert(data->num_vfs);

	if (!xe_has_vram(data->pf_fd)) {
		set_skip_reason(data, "No LMEM\n");
		return;
	}

	if (populate_vf_lmem_sizes(data))
		/* skip reason set in populate_vf_lmem_sizes */
		return;
}

static void lmem_subcheck_prepare_vf(int vf_id, struct subcheck_data *data)
{
	struct lmem_data *ldata = (struct lmem_data *)data;

	if (data->stop_reason)
		return;

	igt_assert(vf_id > 0 && vf_id <= data->num_vfs);

	if (!lmem_mmap_write_munmap(data->pf_fd, vf_id,
				    ldata->vf_lmem_size[vf_id], vf_id)) {
		set_abort_reason(data, "LMEM write failed on VF%u\n", vf_id);
	}
}

static void lmem_subcheck_verify_vf(int vf_id, int flr_vf_id, struct subcheck_data *data)
{
	struct lmem_data *ldata = (struct lmem_data *)data;
	char expected = (vf_id == flr_vf_id) ? 0 : vf_id;

	if (data->stop_reason)
		return;

	if (!lmem_contains_expected_values(data->pf_fd, vf_id,
					   ldata->vf_lmem_size[vf_id], expected)) {
		set_fail_reason(data,
				"LMEM check after VF%u FLR failed on VF%u\n",
				flr_vf_id, vf_id);
	}
}

static void lmem_subcheck_cleanup(struct subcheck_data *data)
{
	struct lmem_data *ldata = (struct lmem_data *)data;

	free(ldata->vf_lmem_size);
}

#define SCRATCH_REG		0x190240
#define SCRATCH_REG_COUNT	4
#define MED_SCRATCH_REG		0x190310
#define MED_SCRATCH_REG_COUNT	4

struct regs_data {
	struct subcheck_data base;
	uint32_t reg_addr;
	int reg_count;
};

static void regs_subcheck_init(struct subcheck_data *data)
{
	struct regs_data *rdata = (struct regs_data *)data;

	if (!xe_has_media_gt(data->pf_fd) &&
	    rdata->reg_addr == MED_SCRATCH_REG) {
		set_skip_reason(data, "No media GT\n");
	}
}

static void regs_subcheck_prepare_vf(int vf_id, struct subcheck_data *data)
{
	struct regs_data *rdata = (struct regs_data *)data;
	struct xe_mmio *mmio = xe_mmio_for_vf(vf_id);
	uint8_t tile = data->tile;
	uint32_t reg;
	int i;

	if (data->stop_reason)
		return;

	for (i = 0; i < rdata->reg_count; i++) {
		reg = rdata->reg_addr + i * 4;

		xe_mmio_tile_write32(mmio, tile, reg, vf_id);
		if (xe_mmio_tile_read32(mmio, tile, reg) != vf_id) {
			set_abort_reason(data, "Registers write/read check failed on VF%u\n",
					 vf_id);
			return;
		}
	}
}

static void regs_subcheck_verify_vf(int vf_id, int flr_vf_id, struct subcheck_data *data)
{
	struct regs_data *rdata = (struct regs_data *)data;
	uint32_t expected = (vf_id == flr_vf_id) ? 0 : vf_id;
	struct xe_mmio *mmio = xe_mmio_for_vf(vf_id);
	uint32_t reg;
	int i;

	if (data->stop_reason)
		return;

	for (i = 0; i < rdata->reg_count; i++) {
		reg = rdata->reg_addr + i * 4;

		if (xe_mmio_tile_read32(mmio, data->tile, reg) != expected) {
			set_fail_reason(data,
					"Registers check after VF%u FLR failed on VF%u\n",
					flr_vf_id, vf_id);
			return;
		}
	}
}

static void regs_subcheck_cleanup(struct subcheck_data *data)
{
}

static void clear_tests(int pf_fd, int num_vfs, flr_exec_strategy exec_strategy)
{
	const uint8_t num_tiles = xe_tiles_count(pf_fd);
	struct subcheck_data base;
	struct ggtt_data gdata[num_tiles];
	struct lmem_data ldata[num_tiles];
	struct regs_data scratch_data[num_tiles];
	struct regs_data media_scratch_data[num_tiles];
	const unsigned int subcheck_count = 4;
	const unsigned int num_checks =	subcheck_count * num_tiles;
	struct subcheck checks[num_checks];
	unsigned int i = 0, t;

	xe_for_each_tile(pf_fd, t) {
		igt_assert_lt(i, num_tiles);
		base = (struct subcheck_data){ .pf_fd = pf_fd,
					       .num_vfs = num_vfs,
					       .tile = t };

		gdata[i] = (struct ggtt_data){
			.base = base,
		};
		checks[i * subcheck_count + 0] = (struct subcheck){
			.data = (struct subcheck_data *)&gdata[i],
			.name = "clear-ggtt",
			.init = ggtt_subcheck_init,
			.prepare_vf = ggtt_subcheck_prepare_vf,
			.verify_vf = ggtt_subcheck_verify_vf,
			.cleanup = ggtt_subcheck_cleanup
		};

		ldata[i] = (struct lmem_data){
			.base = base,
		};
		checks[i * subcheck_count + 1] = (struct subcheck){
			.data = (struct subcheck_data *)&ldata[i],
			.name = "clear-lmem",
			.init = lmem_subcheck_init,
			.prepare_vf = lmem_subcheck_prepare_vf,
			.verify_vf = lmem_subcheck_verify_vf,
			.cleanup = lmem_subcheck_cleanup
		};

		scratch_data[i] = (struct regs_data){
			.base = base,
			.reg_addr = SCRATCH_REG,
			.reg_count = SCRATCH_REG_COUNT,
		};
		checks[i * subcheck_count + 2] = (struct subcheck){
			.data = (struct subcheck_data *)&scratch_data[i],
			.name = "clear-scratch-regs",
			.init = regs_subcheck_init,
			.prepare_vf = regs_subcheck_prepare_vf,
			.verify_vf = regs_subcheck_verify_vf,
			.cleanup = regs_subcheck_cleanup
		};

		media_scratch_data[i] = (struct regs_data){
			.base = base,
			.reg_addr = MED_SCRATCH_REG,
			.reg_count = MED_SCRATCH_REG_COUNT,
		};
		checks[i * subcheck_count + 3] = (struct subcheck){
			.data = (struct subcheck_data *)&media_scratch_data[i],
			.name = "clear-media-scratch-regs",
			.init = regs_subcheck_init,
			.prepare_vf = regs_subcheck_prepare_vf,
			.verify_vf = regs_subcheck_verify_vf,
			.cleanup = regs_subcheck_cleanup
		};
		i++;
	}
	igt_assert_eq(i, num_tiles);
	igt_assert_eq(i * subcheck_count, num_checks);

	verify_flr(pf_fd, num_vfs, checks, num_checks, exec_strategy);
}

static int opt_handler(int opt, int opt_index, void *data)
{
	char *end = NULL;
	long val;

	switch (opt) {
	case 'w':
		errno = 0;
		val = strtol(optarg, &end, 0);
		if (errno || !end || *end != '\0' || val < 0 || val > INT_MAX)
			return IGT_OPT_HANDLER_ERROR;
		g_wait_flr_ms = (int)val;
		igt_info("Using wait_flr_ms=%d\n", g_wait_flr_ms);
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

static const struct option long_options[] = {
	{ .name = "wait-flr-ms", .has_arg = true, .val = 'w', },
	{},
};

static const char help_str[] =
	"  --wait-flr-ms=MS\tSleep MS milliseconds after VF reset sysfs write (default: 200)\n";

int igt_main_args("w:", long_options, help_str, opt_handler, NULL)
{
	int pf_fd;
	bool autoprobe;

	igt_fixture() {
		pf_fd = drm_open_driver(DRIVER_XE);
		igt_require(igt_sriov_is_pf(pf_fd));
		igt_require(igt_sriov_get_enabled_vfs(pf_fd) == 0);
		autoprobe = igt_sriov_is_driver_autoprobe_enabled(pf_fd);
	}

	igt_describe("Verify LMEM, GGTT, and SCRATCH_REGS are properly cleared after VF1 FLR");
	igt_subtest("flr-vf1-clear") {
		clear_tests(pf_fd, 1, execute_sequential_flr);
	}

	igt_describe("Perform sequential FLR on each VF, verifying that LMEM, GGTT, and SCRATCH_REGS are cleared only on the reset VF.");
	igt_subtest("flr-each-isolation") {
		unsigned int total_vfs = igt_sriov_get_total_vfs(pf_fd);

		igt_require(total_vfs > 1);

		clear_tests(pf_fd, total_vfs > 3 ? 3 : total_vfs, execute_sequential_flr);
	}

	igt_describe("Perform FLR on all VFs in parallel, ensuring correct behavior during simultaneous resets.");
	igt_subtest("flr-vfs-parallel") {
		unsigned int total_vfs = igt_sriov_get_total_vfs(pf_fd);

		igt_require(total_vfs > 1);

		clear_tests(pf_fd, total_vfs, execute_parallel_flr);
	}

	igt_describe("Initiate FLR twice in parallel on same VF.");
	igt_subtest("flr-twice") {
		clear_tests(pf_fd, 1, execute_parallel_flr_twice);
	}

	igt_fixture() {
		igt_sriov_disable_vfs(pf_fd);
		/* abort to avoid execution of next tests with enabled VFs */
		igt_abort_on_f(igt_sriov_get_enabled_vfs(pf_fd) > 0, "Failed to disable VF(s)");
		autoprobe ? igt_sriov_enable_driver_autoprobe(pf_fd) :
			    igt_sriov_disable_driver_autoprobe(pf_fd);
		igt_abort_on_f(autoprobe != igt_sriov_is_driver_autoprobe_enabled(pf_fd),
			       "Failed to restore sriov_drivers_autoprobe value\n");
		close(pf_fd);
	}
}
