// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include <fcntl.h>
#include <linux_scaffold.h>

#include "igt_syncobj.h"
#include "intel_pat.h"
#include "ioctl_wrappers.h"
#include "xe/xe_gt.h"
#include "xe/xe_ioctl.h"

#define OVERFLOW_PRL_SIZE 512

/**
 * TEST: xe_page_reclaim
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: VM bind
 * Functionality: Page Reclamation
 * Test category: functionality test
 */
struct xe_prl_stats {
	int prl_4k_entry_count;
	int prl_64k_entry_count;
	int prl_2m_entry_count;
	int prl_issued_count;
	int prl_aborted_count;
};

/*
 * PRL is only active on the render GT (gt0); media tiles do not participate
 * in page reclamation. Callers typically pass gt=0.
 */
static struct xe_prl_stats get_prl_stats(int fd, int gt)
{
	struct xe_prl_stats stats = {0};

	stats.prl_4k_entry_count = xe_gt_stats_get_count(fd, gt, "prl_4k_entry_count");
	stats.prl_64k_entry_count = xe_gt_stats_get_count(fd, gt, "prl_64k_entry_count");
	stats.prl_2m_entry_count = xe_gt_stats_get_count(fd, gt, "prl_2m_entry_count");
	stats.prl_issued_count = xe_gt_stats_get_count(fd, gt, "prl_issued_count");
	stats.prl_aborted_count = xe_gt_stats_get_count(fd, gt, "prl_aborted_count");

	return stats;
}

#define XE2_L3_POLICY		GENMASK(5, 4)
#define L3_CACHE_POLICY_XD	1

static int get_xd_pat_idx(int fd)
{
	uint16_t dev_id = intel_get_drm_devid(fd);
	struct intel_pat_cache pat_config = {};
	int32_t parsed;
	int i;

	if (intel_graphics_ver(dev_id) < IP_VER(20, 0))
		return -1;

	parsed = xe_get_pat_sw_config(fd, &pat_config, 0);
	if (parsed <= 0)
		return -1;

	for (i = 0; i < parsed; i++) {
		if (pat_config.entries[i].rsvd)
			continue;
		if (FIELD_GET(XE2_L3_POLICY, pat_config.entries[i].pat) == L3_CACHE_POLICY_XD)
			return i;
	}

	return -1;
}

static void log_prl_stat_diff(struct xe_prl_stats *stats_before, struct xe_prl_stats *stats_after)
{
	igt_debug("PRL stats diff: 4K: %d->%d, 64K: %d->%d, 2M: %d -> %d, issued: %d->%d, aborted: %d->%d\n",
		  stats_before->prl_4k_entry_count,
		  stats_after->prl_4k_entry_count,
		  stats_before->prl_64k_entry_count,
		  stats_after->prl_64k_entry_count,
		  stats_before->prl_2m_entry_count,
		  stats_after->prl_2m_entry_count,
		  stats_before->prl_issued_count,
		  stats_after->prl_issued_count,
		  stats_before->prl_aborted_count,
		  stats_after->prl_aborted_count);
}

/* Compare differences between stats and determine if expected */
static void compare_prl_stats(struct xe_prl_stats *before, struct xe_prl_stats *after,
			      struct xe_prl_stats *expected)
{
	log_prl_stat_diff(before, after);

	igt_assert_eq(after->prl_4k_entry_count - before->prl_4k_entry_count,
		      expected->prl_4k_entry_count);
	igt_assert_eq(after->prl_64k_entry_count - before->prl_64k_entry_count,
		      expected->prl_64k_entry_count);
	igt_assert_eq(after->prl_2m_entry_count - before->prl_2m_entry_count,
		      expected->prl_2m_entry_count);
	igt_assert_eq(after->prl_issued_count - before->prl_issued_count,
		      expected->prl_issued_count);
	igt_assert_eq(after->prl_aborted_count - before->prl_aborted_count,
		      expected->prl_aborted_count);
}

static void xe_vm_null_bind_sync(int fd, uint32_t vm, uint64_t addr, uint64_t size)
{
	struct drm_xe_sync sync = {
		.type = DRM_XE_SYNC_TYPE_SYNCOBJ,
		.flags = DRM_XE_SYNC_FLAG_SIGNAL,
		.handle = syncobj_create(fd, 0),
	};

	xe_vm_bind_async_flags(fd, vm, 0, 0, 0, addr, size, &sync, 1,
			       DRM_XE_VM_BIND_FLAG_NULL);
	igt_assert(syncobj_wait(fd, &sync.handle, 1, INT64_MAX, 0, NULL));
	syncobj_destroy(fd, sync.handle);
}

/* Helper with more flexibility on unbinding and offsets */
static void vma_range_list_with_unbind_and_offsets(int fd, const uint64_t *vma_sizes, unsigned int n_vmas,
				 uint64_t start_addr, uint64_t unbind_size, const uint64_t *vma_offsets)
{
	uint32_t vm;
	uint32_t *bos;
	uint64_t addr;

	igt_assert(vma_sizes);
	igt_assert(n_vmas);

	vm = xe_vm_create(fd, 0, 0);

	bos = calloc(n_vmas, sizeof(*bos));
	igt_assert(bos);

	addr = start_addr;
	for (unsigned int i = 0; i < n_vmas; i++) {
		igt_assert(vma_sizes[i]);

		bos[i] = xe_bo_create(fd, 0, vma_sizes[i], system_memory(fd), 0);
		if (vma_offsets)
			addr = start_addr + vma_offsets[i];
		xe_vm_bind_sync(fd, vm, bos[i], 0, addr, vma_sizes[i]);
		addr += vma_sizes[i];
	}

	/* Unbind the whole contiguous VA span in one operation. */
	xe_vm_unbind_sync(fd, vm, 0, start_addr, unbind_size ? unbind_size : addr - start_addr);

	for (unsigned int i = 0; i < n_vmas; i++)
		gem_close(fd, bos[i]);

	free(bos);
	xe_vm_destroy(fd, vm);
}

/*
 * Takes in an array of vma sizes and allocates/binds individual BOs for each given size,
 * then unbinds them all at once
 */
static void test_vma_ranges_list(int fd, const uint64_t *vma_sizes,
				 unsigned int n_vmas, uint64_t start_addr)
{
	vma_range_list_with_unbind_and_offsets(fd, vma_sizes, n_vmas, start_addr, 0, NULL);
}

/**
 * SUBTEST: basic-mixed
 * Description: Create multiple different sizes of page (4K, 64K, 2M)
 *      GPU VMA ranges, bind them into a VM at unique addresses, then
 *      unbind all to trigger page reclamation on different page sizes
 *      in one page reclaim list.
 */
static void test_vma_ranges_basic_mixed(int fd)
{
	struct xe_prl_stats stats_before, stats_after, expected_stats = { 0 };
	const uint64_t num_4k_pages = 16;
	const uint64_t num_64k_pages = 31;
	const uint64_t num_2m_pages = 2;
	uint64_t *sizes = calloc(num_4k_pages + num_64k_pages + num_2m_pages, sizeof(uint64_t));
	int count = 0;

	igt_assert(sizes);
	for (int i = 0; i < num_4k_pages; i++)
		sizes[count++] = SZ_4K;

	for (int i = 0; i < num_64k_pages; i++)
		sizes[count++] = SZ_64K;

	for (int i = 0; i < num_2m_pages; i++)
		sizes[count++] = SZ_2M;

	expected_stats.prl_4k_entry_count = num_4k_pages;
	expected_stats.prl_64k_entry_count = num_64k_pages;
	expected_stats.prl_2m_entry_count = num_2m_pages;
	expected_stats.prl_issued_count = 1;
	expected_stats.prl_aborted_count = 0;

	stats_before = get_prl_stats(fd, 0);
	test_vma_ranges_list(fd, sizes, count, 1ull << 30);
	stats_after = get_prl_stats(fd, 0);

	free(sizes);
	compare_prl_stats(&stats_before, &stats_after, &expected_stats);
}

/* Helper to calculate alignment filler pages needed */
struct alignment_fillers {
	uint64_t gap_to_64k;
	uint64_t remaining_gap;
};

static struct alignment_fillers calculate_alignment_fillers(uint64_t start_addr,
							    uint64_t current_offset,
							    uint64_t page_size)
{
	struct alignment_fillers fillers = {0};
	uint64_t current_addr = start_addr + current_offset;
	uint64_t misalignment = current_addr % page_size;
	uint64_t gap, misalignment_64k;

	if (!misalignment)
		return fillers;

	gap = page_size - misalignment;

	/*
	 * Fill the alignment gap using a two-level strategy to match the
	 * GPU page table hierarchy (4K → 64K → page_size):
	 * 1. Use 4K pages to reach the next 64K boundary (if the current
	 *    address is not already 64K-aligned).
	 * 2. Use 64K pages for the remaining gap up to page_size alignment
	 *    (this remainder is always a multiple of 64K).
	 */
	misalignment_64k = current_addr % SZ_64K;
	if (misalignment_64k) {
		/* Not 64K aligned, fill with 4K pages up to next 64K boundary */
		fillers.gap_to_64k = SZ_64K - misalignment_64k;
		if (fillers.gap_to_64k > gap)
			fillers.gap_to_64k = gap;
	}

	fillers.remaining_gap = gap - fillers.gap_to_64k;
	return fillers;
}

/**
 * SUBTEST: random
 * Description: Create a random mix of page sizes (4K, 64K, 2M) with
 *      proper alignment handling. Larger pages are aligned by inserting
 *      filler pages (64K and 4K) as needed to test random page size
 *      combinations in page reclamation.
 */
static void test_vma_range_random(int fd)
{
	struct xe_prl_stats stats_before, stats_after, expected_stats = { 0 };
	const int num_random_pages = 10; /* Total number of random pages to generate */
	struct alignment_fillers fillers;
	uint64_t *sizes;
	uint64_t *random_pages;
	uint64_t start_addr = 1ull << 30; /* Start at 1GB aligned */
	int count_4k = 0, count_64k = 0, count_2m = 0;
	uint64_t current_offset = 0;
	uint64_t remainder, j;
	int idx = 0;
	int i, rand_val;

	/* Generate random page sizes */
	random_pages = calloc(num_random_pages, sizeof(uint64_t));
	igt_assert(random_pages);

	for (i = 0; i < num_random_pages; i++) {
		rand_val = random() % 100;

		/* Weight the distribution: 50% 4K, 30% 64K, 20% 2M */
		if (rand_val < 50)
			random_pages[i] = SZ_4K;
		else if (rand_val < 80)
			random_pages[i] = SZ_64K;
		else
			random_pages[i] = SZ_2M;
	}

	/* Over-allocate: worst case is 47 pages per random page (15×4K + 31×64K + 1×2M) */
	sizes = calloc(num_random_pages * 47, sizeof(uint64_t));
	igt_assert(sizes);

	/* Populate sizes array in a single pass while tracking counts */
	for (i = 0; i < num_random_pages; i++) {
		fillers = calculate_alignment_fillers(start_addr,
						      current_offset,
						      random_pages[i]);

		if (fillers.gap_to_64k || fillers.remaining_gap) {
			/* Fill gap to 64K boundary with 4K pages */
			for (j = 0; j < fillers.gap_to_64k; j += SZ_4K) {
				sizes[idx++] = SZ_4K;
				current_offset += SZ_4K;
				count_4k++;
			}

			/* Fill remainder with 64K pages */
			remainder = fillers.remaining_gap;
			while (remainder >= SZ_64K) {
				sizes[idx++] = SZ_64K;
				current_offset += SZ_64K;
				remainder -= SZ_64K;
				count_64k++;
			}

			/* After 64K alignment, remainder should always be 0 */
			igt_assert_eq(remainder, 0);
		}

		sizes[idx++] = random_pages[i];
		current_offset += random_pages[i];

		switch (random_pages[i]) {
		case SZ_4K:
			count_4k++;
			break;
		case SZ_64K:
			count_64k++;
			break;
		case SZ_2M:
			count_2m++;
			break;
		}
	}

	igt_assert_f(idx < OVERFLOW_PRL_SIZE,
		     "Random test generated %d entries, exceeds PRL limit %d\n",
		     idx, OVERFLOW_PRL_SIZE);

	/* Set expected stats based on tracked counts */
	expected_stats.prl_4k_entry_count = count_4k;
	expected_stats.prl_64k_entry_count = count_64k;
	expected_stats.prl_2m_entry_count = count_2m;
	expected_stats.prl_issued_count = 1;
	expected_stats.prl_aborted_count = 0;

	igt_debug("Random test generated: %d total pages (%d 4K, %d 64K, %d 2M)\n",
		  idx, count_4k, count_64k, count_2m);

	stats_before = get_prl_stats(fd, 0);
	test_vma_ranges_list(fd, sizes, idx, start_addr);
	stats_after = get_prl_stats(fd, 0);

	free(random_pages);
	free(sizes);
	compare_prl_stats(&stats_before, &stats_after, &expected_stats);
}

/**
 * SUBTEST: prl-invalidate-full
 * Description: Create 512 4K page entries at the maximum page reclaim list
 *      size boundary and bind them into a VM.
 *      Expects to trigger a fallback to full PPC flush due to page reclaim
 *      list size limitations (512 entries max).
 *
 * SUBTEST: prl-max-entries
 * Description: Create the maximum page reclaim list without overflow
 *      bind them into a VM.
 *      Expects no fallback to PPC flush due to page reclaim
 *      list size limitations (512 entries max).
 */
static void test_vma_ranges_prl_entries(int fd, unsigned int num_entries,
					int expected_issued, int expected_aborted)
{
	struct xe_prl_stats stats_before, stats_after, expected_stats = { 0 };
	const uint64_t page_size = SZ_4K;
	/* Start address aligned but offset by a page to ensure no large PTE are created */
	uint64_t addr = (1ull << 30) + page_size;

	/* Capped at OVERFLOW_PRL_SIZE - 1: on overflow the last entry triggers abort */
	expected_stats.prl_4k_entry_count = min_t(int, num_entries, OVERFLOW_PRL_SIZE - 1);
	expected_stats.prl_64k_entry_count = 0;
	expected_stats.prl_2m_entry_count = 0;
	expected_stats.prl_issued_count = expected_issued;
	expected_stats.prl_aborted_count = expected_aborted;

	stats_before = get_prl_stats(fd, 0);
	test_vma_ranges_list(fd, &(uint64_t){page_size * num_entries}, 1, addr);
	stats_after = get_prl_stats(fd, 0);
	compare_prl_stats(&stats_before, &stats_after, &expected_stats);
}

/*
 * Bind the BOs to multiple VA ranges and unbind all VA with one range.
 * BO size is chosen as the maximum of the requested VMA sizes.
 */
static void test_many_ranges_one_bo(int fd,
				    const uint64_t vma_size,
				    unsigned int n_vmas,
				    uint64_t start_addr)
{
	uint32_t vm;
	uint64_t addr;
	uint32_t bo;

	igt_assert(n_vmas);

	vm = xe_vm_create(fd, 0, 0);

	igt_assert(vma_size);
	bo = xe_bo_create(fd, 0, vma_size, system_memory(fd), 0);

	addr = start_addr;
	for (unsigned int i = 0; i < n_vmas; i++) {
		/* Bind the same BO (offset 0) at a new VA location */
		xe_vm_bind_sync(fd, vm, bo, 0, addr, vma_size);
		addr += vma_size;
	}

	/* Unbind all VMAs */
	xe_vm_unbind_sync(fd, vm, 0, start_addr, addr - start_addr);

	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}

/**
 * SUBTEST: many-vma-same-bo
 * Description: Create multiple 4K page VMA ranges bound to the same BO,
 *      bind them into a VM at unique addresses, then unbind all to trigger
 *      page reclamation handling when the same BO is bound to multiple
 *      virtual addresses.
 */
static void test_vma_ranges_many_vma_same_bo(int fd, uint64_t vma_size, unsigned int n_vmas)
{
	struct xe_prl_stats stats_before, stats_after, expected_stats = { 0 };

	expected_stats.prl_4k_entry_count = n_vmas;
	expected_stats.prl_issued_count = 1;

	stats_before = get_prl_stats(fd, 0);
	test_many_ranges_one_bo(fd, vma_size, n_vmas, 1ull << 30);
	stats_after = get_prl_stats(fd, 0);
	compare_prl_stats(&stats_before, &stats_after, &expected_stats);
}

/**
 * SUBTEST: invalid-1g
 * Description: Create a 1G page VMA followed by a 4K page VMA to test
 *      handling of 1G page mappings during page reclamation.
 *      Expected is to fallback to invalidation.
 */
static void test_vma_range_invalid_1g(int fd)
{
	struct xe_prl_stats stats_before, stats_after, expected_stats = { 0 };
	static const uint64_t sizes[] = {
		SZ_1G,
		SZ_4K,
	};
	int delta_4k, delta_64k, delta_2m, delta_issued, delta_aborted;
	bool expected_2m_entries, all_entries_dropped;

	/* 1G page broken into 512 2M pages, but it should invalidate the last entry */
	expected_stats.prl_2m_entry_count = OVERFLOW_PRL_SIZE - 1;
	/* No page size because PRL should be invalidated before the second page */
	expected_stats.prl_4k_entry_count = 0;
	expected_stats.prl_issued_count = 0;
	expected_stats.prl_aborted_count = 1;

	stats_before = get_prl_stats(fd, 0);
	/* Offset 2G to avoid alignment issues */
	test_vma_ranges_list(fd, sizes, ARRAY_SIZE(sizes), SZ_2G);
	stats_after = get_prl_stats(fd, 0);
	log_prl_stat_diff(&stats_before, &stats_after);

	/*
	 * Depending on page placement, 1G page directory could be dropped from page walk
	 * which would not generate any entries
	 */
	delta_4k = stats_after.prl_4k_entry_count - stats_before.prl_4k_entry_count;
	delta_64k = stats_after.prl_64k_entry_count - stats_before.prl_64k_entry_count;
	delta_2m = stats_after.prl_2m_entry_count - stats_before.prl_2m_entry_count;
	delta_issued = stats_after.prl_issued_count - stats_before.prl_issued_count;
	delta_aborted = stats_after.prl_aborted_count - stats_before.prl_aborted_count;
	expected_2m_entries = (delta_2m == expected_stats.prl_2m_entry_count);
	all_entries_dropped = (delta_2m == 0 && delta_64k == 0 && delta_4k == 0);

	igt_assert_eq(delta_issued, expected_stats.prl_issued_count);
	igt_assert_eq(delta_aborted, expected_stats.prl_aborted_count);
	igt_assert_eq(delta_4k, expected_stats.prl_4k_entry_count);
	igt_assert(expected_2m_entries || all_entries_dropped);
}

/**
 * SUBTEST: pde-vs-pd
 * Description: Test case to trigger invalidation of both PDE (2M pages)
 *      and PD (page directory filled with 64K pages) to determine correct
 *      handling of both cases for PRL.
 */
static void test_vma_ranges_pde_vs_pd(int fd)
{
	struct xe_prl_stats stats_before, stats_after, expected_stats = { 0 };
	/* Ensure no alignment issue by using 1G */
	uint64_t start_addr = 1ull << 30;
	/* 32 pages of 64K to fill one page directory */
	static const unsigned int num_pages = SZ_2M / SZ_64K;
	static const uint64_t size_pde[] = {
		SZ_2M,
	};
	uint64_t size_pd[num_pages];

	for (int i = 0; i < num_pages; i++)
		size_pd[i] = SZ_64K;

	expected_stats = (struct xe_prl_stats) {
		.prl_64k_entry_count = num_pages,
		.prl_issued_count = 1,
	};
	stats_before = get_prl_stats(fd, 0);
	test_vma_ranges_list(fd, size_pd, ARRAY_SIZE(size_pd), start_addr);
	stats_after = get_prl_stats(fd, 0);
	compare_prl_stats(&stats_before, &stats_after, &expected_stats);

	expected_stats = (struct xe_prl_stats) {
		.prl_2m_entry_count = 1,
		.prl_issued_count = 1,
	};
	stats_before = get_prl_stats(fd, 0);
	test_vma_ranges_list(fd, size_pde, ARRAY_SIZE(size_pde), start_addr);
	stats_after = get_prl_stats(fd, 0);
	compare_prl_stats(&stats_before, &stats_after, &expected_stats);
}

/**
 * SUBTEST: boundary-split
 * Description: Test case to trigger PRL generation beyond a page size alignment
 *      to ensure correct handling of PRL entries that span page size boundaries.
 */
static void test_boundary_split(int fd)
{
	struct xe_prl_stats stats_before, stats_after, expected_stats = { 0 };
	/* Dangle a page past the boundary with a combination of address offset and size */
	uint64_t size_boundary = 64 * SZ_2M + SZ_4K;
	uint64_t addr = (1ull << 30) + 64 * SZ_2M;

	expected_stats.prl_4k_entry_count = 1;
	expected_stats.prl_64k_entry_count = 0;
	expected_stats.prl_2m_entry_count = 64;
	expected_stats.prl_issued_count = 1;
	expected_stats.prl_aborted_count = 0;

	stats_before = get_prl_stats(fd, 0);
	test_vma_ranges_list(fd, &(uint64_t){size_boundary}, 1, addr);
	stats_after = get_prl_stats(fd, 0);
	compare_prl_stats(&stats_before, &stats_after, &expected_stats);
}

/**
 * SUBTEST: binds-1g-partial
 * Description: Bind a 1G VMA and a 2M VMA into a VM and unbind only
 *      the 1G range to verify that decomposing a 1G mapping into its
 *      constituent 2M PRL entries overflows the PRL capacity limit,
 *      triggering a full TLB invalidation fallback (aborted PRL) instead
 *      of a targeted page reclaim list flush.
 */
static void test_binds_1g_partial(int fd)
{
	struct xe_prl_stats stats_before, stats_after, expected_stats = { 0 };

	uint64_t sizes[]   = { SZ_1G, SZ_2M };
	uint64_t offsets[] = { 0, SZ_1G };
	int count = ARRAY_SIZE(sizes);

	expected_stats.prl_4k_entry_count = 0;
	expected_stats.prl_64k_entry_count = 0;
	expected_stats.prl_2m_entry_count = 0;
	expected_stats.prl_issued_count = 0;
	expected_stats.prl_aborted_count = 1;

	stats_before = get_prl_stats(fd, 0);
	vma_range_list_with_unbind_and_offsets(fd, sizes, count, (1ull << 30), SZ_1G + SZ_2M, offsets);
	stats_after = get_prl_stats(fd, 0);
	compare_prl_stats(&stats_before, &stats_after, &expected_stats);
}

/**
 * SUBTEST: pat-index-xd
 * Description: Create a VM binding with a BO that has PAT INDEX with XD
 *      (transient display) property to test page reclamation
 *      with transient cache entries on XE2+ platforms.
 */
static void test_pat_index_xd(int fd)
{
	struct xe_prl_stats stats_before, stats_after, expected_stats = { 0 };
	uint32_t vm, bo;
	uint64_t size = SZ_4K;
	uint64_t addr = 1ull << 30;
	int pat_idx_xd, err;
	struct drm_xe_sync sync = {
		.type = DRM_XE_SYNC_TYPE_SYNCOBJ,
		.flags = DRM_XE_SYNC_FLAG_SIGNAL,
	};

	pat_idx_xd = get_xd_pat_idx(fd);
	igt_require_f(pat_idx_xd >= 0, "XD PAT index not available on this platform\n");

	vm = xe_vm_create(fd, 0, 0);
	bo = xe_bo_create_caching(fd, 0, size, system_memory(fd), 0,
				  DRM_XE_GEM_CPU_CACHING_WC);

	/* Bind with XD PAT index - synchronous operation */
	sync.handle = syncobj_create(fd, 0);
	err = __xe_vm_bind(fd, vm, 0, bo, 0, addr,
			   size, DRM_XE_VM_BIND_OP_MAP, 0, &sync, 1, 0,
			   pat_idx_xd, 0);
	igt_assert_eq(err, 0);
	igt_assert(syncobj_wait(fd, &sync.handle, 1, INT64_MAX, 0, NULL));
	syncobj_destroy(fd, sync.handle);

	/*
	 * Page reclamation should skip over the XD pat vma pages.
	 * PRL is still issued because pages are still valid, just handled
	 * elsewhere so no invalidation required to ensure not squashing valid
	 * PRL entries from other VMAs.
	 */
	expected_stats.prl_4k_entry_count = 0;
	expected_stats.prl_64k_entry_count = 0;
	expected_stats.prl_2m_entry_count = 0;
	expected_stats.prl_issued_count = 1;
	expected_stats.prl_aborted_count = 0;

	stats_before = get_prl_stats(fd, 0);
	xe_vm_unbind_sync(fd, vm, 0, addr, size);
	stats_after = get_prl_stats(fd, 0);

	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
	compare_prl_stats(&stats_before, &stats_after, &expected_stats);
}

/**
 * SUBTEST: binds-large-split
 * Description: Bind a large BO (256MB + 4K) split across two adjacent
 *      VM bind operations at non-2M-aligned boundaries, then unbind each
 *      range separately to verify that page reclamation correctly handles
 *      split binds producing a mix of 2M and 4K PRL entries across two
 *      distinct page reclaim list operations.
 */
static void test_binds_large_split(int fd)
{
	struct xe_prl_stats stats_before, stats_after, expected_stats = { 0 };

	uint64_t bo_size = SZ_256M;
	uint64_t split_offset = SZ_4K;
	uint32_t bo, vm;

	uint64_t addr = (1ull << 30);

	expected_stats.prl_4k_entry_count = 1;
	expected_stats.prl_64k_entry_count = 0;
	expected_stats.prl_2m_entry_count = 128;
	expected_stats.prl_issued_count = 2;
	expected_stats.prl_aborted_count = 0;

	stats_before = get_prl_stats(fd, 0);

	vm = xe_vm_create(fd, 0, 0);
	/* Slightly larger BO to see behavior of split */
	bo = xe_bo_create(fd, 0, bo_size + split_offset, system_memory(fd), 0);
	xe_vm_bind_sync(fd, vm, bo, 0, addr, bo_size / 2);
	xe_vm_bind_sync(fd, vm, bo, bo_size / 2, addr + bo_size / 2, bo_size / 2 + split_offset);
	xe_vm_unbind_sync(fd, vm, 0, addr, bo_size / 2);
	xe_vm_unbind_sync(fd, vm, 0, addr + bo_size / 2, bo_size / 2 + split_offset);

	stats_after = get_prl_stats(fd, 0);

	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);

	compare_prl_stats(&stats_before, &stats_after, &expected_stats);
}

/**
 * SUBTEST: binds-full-pd
 * Description: Fill almost an entire 1G page directory (1G minus 4K)
 *      by splitting a single BO across two adjacent VM bind operations,
 *      then unbind each range separately to verify correct page reclamation
 *      when most entries of a page directory are present, producing a mix
 *      of 2M, 64K, and 4K PRL entries across two page reclaim list
 *      operations.
 */
static void test_binds_full_pd(int fd)
{
	struct xe_prl_stats stats_before, stats_after, expected_stats = { 0 };
	uint32_t bo, vm;
	uint64_t addr = (1ull << 30);

	expected_stats.prl_4k_entry_count = 15;
	expected_stats.prl_64k_entry_count = 31;
	expected_stats.prl_2m_entry_count = 511;
	expected_stats.prl_issued_count = 2;
	expected_stats.prl_aborted_count = 0;

	stats_before = get_prl_stats(fd, 0);

	vm = xe_vm_create(fd, 0, 0);
	bo = xe_bo_create(fd, 0, SZ_1G - SZ_4K, system_memory(fd), 0);
	xe_vm_bind_sync(fd, vm, bo, 0, addr, SZ_512M);
	xe_vm_bind_sync(fd, vm, bo, SZ_512M, addr + SZ_512M, SZ_512M - SZ_4K);
	xe_vm_unbind_sync(fd, vm, 0, addr, SZ_512M);
	xe_vm_unbind_sync(fd, vm, 0, addr + SZ_512M, SZ_512M - SZ_4K);

	stats_after = get_prl_stats(fd, 0);

	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);

	compare_prl_stats(&stats_before, &stats_after, &expected_stats);
}

/**
 * SUBTEST: binds-null-vma
 * Description: Verify handling of null VMAs by creating a VM with real
 *      BO mappings followed by a large null VMA mapping, then trigger
 *      page reclamation across the entire range.
 */
static void test_binds_null_vma(int fd)
{
	struct xe_prl_stats stats_before, stats_after, expected_stats = { 0 };
	uint32_t bo, vm;
	/* 512G aligned */
	uint64_t addr = 0;

	/*
	 * Layout:
	 *   [0,      2M)      : real 2M BO   → allocates the level-3[0] subtree
	 *   [2M,  512G)       : null VMA     → fills the rest of the level-3[0] span
	 *   UNMAP [0, 512G)   → walk covers exactly one level-3 entry (512G each)
	 *
	 * Only the real 2M BO produces a PRL entry; the null VMA has no
	 * hardware PTEs and is skipped by the page-reclaim walker.
	 */
	expected_stats.prl_4k_entry_count = 0;
	expected_stats.prl_64k_entry_count = 0;
	expected_stats.prl_2m_entry_count = 1;
	expected_stats.prl_issued_count = 1;
	expected_stats.prl_aborted_count = 0;

	stats_before = get_prl_stats(fd, 0);

	vm = xe_vm_create(fd, 0, 0);
	bo = xe_bo_create(fd, 0, SZ_2M, system_memory(fd), 0);
	xe_vm_bind_sync(fd, vm, bo, 0, addr, SZ_2M);
	xe_vm_null_bind_sync(fd, vm, addr + SZ_2M, 512ull * SZ_1G - SZ_2M);
	xe_vm_unbind_sync(fd, vm, 0, addr, 512ull * SZ_1G);

	stats_after = get_prl_stats(fd, 0);

	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);

	compare_prl_stats(&stats_before, &stats_after, &expected_stats);
}

int igt_main()
{
	int fd;
	/* Buffer to read debugfs entries boolean */
	char buf[16] = {0};

	igt_fixture() {
		fd = drm_open_driver(DRIVER_XE);

		igt_require_f(igt_debugfs_exists(fd, "page_reclaim_hw_assist", O_RDONLY),
			      "Page Reclamation feature is not supported.\n");

		igt_debugfs_read(fd, "page_reclaim_hw_assist", buf);
		igt_require_f(buf[0] == '1',
			      "Page Reclamation feature is not enabled.\n");

		igt_require_f(xe_gt_stats_get_count(fd, 0, "prl_4k_entry_count") >= 0,
			      "gt_stats is required for Page Reclamation tests.\n");
		igt_srandom();
	}

	igt_subtest("basic-mixed")
		test_vma_ranges_basic_mixed(fd);

	igt_subtest("random")
		test_vma_range_random(fd);

	igt_subtest("prl-invalidate-full")
		test_vma_ranges_prl_entries(fd, OVERFLOW_PRL_SIZE, 0, 1);

	igt_subtest("prl-max-entries")
		test_vma_ranges_prl_entries(fd, OVERFLOW_PRL_SIZE - 1, 1, 0);

	igt_subtest("many-vma-same-bo")
		test_vma_ranges_many_vma_same_bo(fd, SZ_4K, 16);

	igt_subtest("pde-vs-pd")
		test_vma_ranges_pde_vs_pd(fd);

	igt_subtest("invalid-1g")
		test_vma_range_invalid_1g(fd);

	igt_subtest("boundary-split")
		test_boundary_split(fd);

	igt_subtest("binds-1g-partial")
		test_binds_1g_partial(fd);

	igt_subtest("pat-index-xd")
		test_pat_index_xd(fd);

	igt_subtest("binds-large-split")
		test_binds_large_split(fd);

	igt_subtest("binds-full-pd")
		test_binds_full_pd(fd);

	igt_subtest("binds-null-vma")
		test_binds_null_vma(fd);

	igt_fixture()
		drm_close_driver(fd);
}
