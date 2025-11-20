// SPDX-License-Identifier: MIT
/*
 * Copyright(c) 2025 Intel Corporation. All rights reserved.
 */

#include "drmtest.h"
#include "igt_core.h"
#include "igt_sriov_device.h"
#include "intel_vram.h"
#include "xe/xe_sriov_provisioning.h"
#include "xe/xe_query.h"

/**
 * TEST: xe_sriov_vram
 * Category: Core
 * Mega feature: SR-IOV
 * Sub-category: VRAM
 * Functionality: VRAM access
 * Description: Validate VF access to VRAM
 *
 * SUBTEST: vf-access-provisioned
 * Description: Verify that VF can access all the provisioned memory via VRAM BAR
 *
 * SUBTEST: vf-access-beyond
 * Description: Verify that VF cannot access memory beyond what's provisioned via VRAM BAR
 *
 * SUBTEST: vf-access-after-resize-down
 * Description: Verify that VF can access the reprovisioned memory (reduced size) via VRAM BAR
 *
 * SUBTEST: vf-access-after-resize-up
 * Description: Verify that VF can access the reprovisioned memory (increased size) via VRAM BAR
 */

IGT_TEST_DESCRIPTION("Xe tests for VRAM in SR-IOV context");

static bool extended_scope, verbose;
const size_t STEP = SZ_1M;

static uint64_t get_provisioned_vram(unsigned int pf_fd, unsigned int vf_id)
{
	uint64_t size = 0;

	/* TODO: adjust for multitile platforms */
	size = xe_sriov_pf_get_provisioned_quota(pf_fd, XE_SRIOV_SHARED_RES_LMEM, vf_id, 0);

	return size;
}

struct range {
	uint64_t start;
	uint64_t end;
	uint8_t write_val;
	uint8_t read_val;
};

static int update_failing_range(struct range **ranges, unsigned int *nr_ranges, uint64_t offset,
				uint8_t write, uint8_t read)
{
	struct range *new_ranges;

	if (!(*ranges) || (offset - (*ranges)[*nr_ranges - 1].end > STEP)) {
		new_ranges = realloc(*ranges, (*nr_ranges + 1) * sizeof(struct range));
		if (!new_ranges) {
			free(*ranges);
			*ranges = NULL;
			*nr_ranges = 0;
			return -ENOMEM;
		}

		*ranges = new_ranges;
		(*nr_ranges)++;
		(*ranges)[*nr_ranges - 1].start = offset;
		(*ranges)[*nr_ranges - 1].write_val = write;
		(*ranges)[*nr_ranges - 1].read_val = read;
	}

	(*ranges)[*nr_ranges - 1].end = offset;

	return 0;
}

static bool validate_access_basic(struct vram_mapping *vram, unsigned int vf_id, size_t size)
{
	struct range *fail_ranges = NULL, *restore_fail_ranges = NULL;
	unsigned int nr_fail_ranges = 0, nr_restore_fail_ranges = 0;
	uint8_t read, orig;
	unsigned int i;

	for (size_t offset = 0; offset < size; offset += STEP) {
		orig = intel_vram_read8(vram, offset);

		read = intel_vram_write_readback8(vram, offset, vf_id);
		if (read != vf_id) {
			if (verbose)
				igt_debug("VRAM write/read check failed on VF%u "
					  "(offset: %#lx, write: %u, read: %u)\n",
					  vf_id, offset, vf_id, read);
			igt_assert(!update_failing_range(&fail_ranges, &nr_fail_ranges, offset,
							 vf_id, read));
		}

		read = intel_vram_write_readback8(vram, offset, orig);
		if (read != orig) {
			if (verbose)
				igt_debug("Failed to restore original value on VF%u "
					  "(offset: %#lx, original: %u, read: %u)\n",
					  vf_id, offset, orig, read);
			igt_assert(!update_failing_range(&restore_fail_ranges,
							 &nr_restore_fail_ranges, offset,
							 orig, read));
		}
	}

	for (i = 0; i < nr_fail_ranges; i++)
		igt_info("VRAM write/read check failed in range %lx-%lx on VF%u "
			 "(offset: %#lx, write: %u, read: %u)\n",
			 fail_ranges[i].start, fail_ranges[i].end, vf_id,
			 fail_ranges[i].start, fail_ranges[i].write_val, fail_ranges[i].read_val);

	for (i = 0; i < nr_restore_fail_ranges; i++)
		igt_info("Failed to restore original value in range %lx-%lx on VF%u\n "
			 "(offset: %#lx, original: %u, read: %u)\n",
			 restore_fail_ranges[i].start, restore_fail_ranges[i].end, vf_id,
			 restore_fail_ranges[i].start, restore_fail_ranges[i].write_val,
			 restore_fail_ranges[i].read_val);

	if (nr_fail_ranges || nr_restore_fail_ranges) {
		free(fail_ranges);
		free(restore_fail_ranges);
		return false;
	}

	return true;
}

static void access_provisioned(unsigned int pf_fd, unsigned int num_vfs)
{
	struct vram_mapping vram;
	size_t provisioned_vram;
	uint64_t vram_bar_size;
	bool passed = true;

	igt_sriov_disable_driver_autoprobe(pf_fd);
	igt_sriov_enable_vfs(pf_fd, num_vfs);

	for_each_sriov_enabled_vf(pf_fd, vf_id) {
		provisioned_vram = get_provisioned_vram(pf_fd, vf_id);
		igt_debug("VF%u provisioned with %" PRIu64 " bytes of VRAM\n",
			  vf_id, provisioned_vram);

		igt_assert(!intel_vram_bar_size(pf_fd, vf_id, &vram_bar_size));
		igt_debug("VF%u VRAM BAR size: %" PRIu64 "\n", vf_id, vram_bar_size);

		if (vram_bar_size < provisioned_vram) {
			igt_sriov_disable_vfs(pf_fd);
			igt_skip("VRAM BAR size is smaller than provisioned VRAM\n");
		}

		igt_assert(!intel_vram_mmap(pf_fd, vf_id, 0, provisioned_vram,
					    PROT_READ | PROT_WRITE, &vram));

		passed &= validate_access_basic(&vram, vf_id, provisioned_vram);

		intel_vram_munmap(&vram);
	}

	igt_sriov_disable_vfs(pf_fd);

	igt_assert(passed);
}

static bool validate_access_beyond(struct vram_mapping *vram, unsigned int vf_id,
				   size_t provisioned_vram, size_t vram_bar_size)
{
	struct range *fail_ranges = NULL, *restore_fail_ranges = NULL;
	unsigned int nr_fail_ranges = 0, nr_restore_fail_ranges = 0;
	uint8_t read, orig;
	unsigned int i;

	for (size_t offset = provisioned_vram; offset < vram_bar_size; offset += STEP) {
		orig = intel_vram_read8(vram, offset);

		read = intel_vram_write_readback8(vram, offset, vf_id);
		if (read == vf_id) {
			igt_assert(!update_failing_range(&fail_ranges, &nr_fail_ranges, offset,
							 vf_id, read));

			read = intel_vram_write_readback8(vram, offset, orig);
			if (read != orig) {
				if (verbose)
					igt_debug("Failed to restore original value on VF%u "
						  "(offset: %#lx, original: %u, read: %u)\n",
						  vf_id, offset, orig, read);
				igt_assert(!update_failing_range(&restore_fail_ranges,
								 &nr_restore_fail_ranges, offset,
								 orig, read));
			}
		}
	}

	for (i = 0; i < nr_fail_ranges; i++)
		igt_info("Unexpected VRAM write beyond provisioned size in range %lx-%lx on VF%u "
			 "(offset: %#lx, write: %u, read: %u)\n",
			 fail_ranges[i].start, fail_ranges[i].end, vf_id,
			 fail_ranges[i].start, fail_ranges[i].write_val, fail_ranges[i].read_val);

	for (i = 0; i < nr_restore_fail_ranges; i++)
		igt_info("Failed to restore original value in range %lx-%lx on VF%u "
			 "(offset: %#lx, original: %u, read: %u)\n",
			 restore_fail_ranges[i].start, restore_fail_ranges[i].end, vf_id,
			 restore_fail_ranges[i].start, restore_fail_ranges[i].write_val,
			 restore_fail_ranges[i].read_val);

	if (fail_ranges || restore_fail_ranges) {
		free(fail_ranges);
		free(restore_fail_ranges);
		return false;
	}

	return true;
}

static void access_beyond(unsigned int pf_fd, unsigned int num_vfs)
{
	struct vram_mapping vram;
	size_t provisioned_vram;
	uint64_t vram_bar_size;
	bool passed = true;

	igt_sriov_disable_driver_autoprobe(pf_fd);
	igt_sriov_enable_vfs(pf_fd, num_vfs);

	for_each_sriov_enabled_vf(pf_fd, vf_id) {
		provisioned_vram = get_provisioned_vram(pf_fd, vf_id);
		igt_debug("VF%u provisioned with %" PRIu64 " bytes of VRAM\n",
			  vf_id, provisioned_vram);

		igt_assert(!intel_vram_bar_size(pf_fd, vf_id, &vram_bar_size));
		igt_debug("VF%u VRAM BAR size: %" PRIu64 "\n", vf_id, vram_bar_size);

		if (vram_bar_size <= provisioned_vram) {
			igt_sriov_disable_vfs(pf_fd);
			igt_skip("VRAM BAR size is smaller or equal to provisioned VRAM\n");
		}

		igt_assert(!intel_vram_mmap(pf_fd, vf_id, 0, vram_bar_size,
					    PROT_READ | PROT_WRITE, &vram));

		passed &= validate_access_beyond(&vram, vf_id, provisioned_vram, vram_bar_size);

		intel_vram_munmap(&vram);
	}

	igt_sriov_disable_vfs(pf_fd);

	igt_assert(passed);
}

static void resize_and_access(unsigned int pf_fd, bool resize_up)
{
	struct vram_mapping vram;
	size_t provisioned_vram;
	uint64_t vram_bar_size;
	unsigned int total_vfs;
	const unsigned int vf_id = 1;
	bool passed;

	total_vfs = igt_sriov_get_total_vfs(pf_fd);

	igt_sriov_disable_driver_autoprobe(pf_fd);
	igt_sriov_enable_vfs(pf_fd, resize_up ? total_vfs : 1);

	provisioned_vram = get_provisioned_vram(pf_fd, vf_id);
	igt_debug("VF%u provisioned with %" PRIu64 " bytes of VRAM\n", vf_id, provisioned_vram);

	igt_sriov_disable_vfs(pf_fd);
	igt_sriov_enable_vfs(pf_fd, resize_up ? 1 : total_vfs);

	provisioned_vram = get_provisioned_vram(pf_fd, vf_id);
	igt_debug("VF%u provisioned with %" PRIu64 " bytes of VRAM\n", vf_id, provisioned_vram);

	igt_assert(!intel_vram_bar_size(pf_fd, vf_id, &vram_bar_size));
	igt_debug("VF%u VRAM BAR size: %" PRIu64 "\n", vf_id, vram_bar_size);

	if (resize_up && vram_bar_size < provisioned_vram) {
		igt_sriov_disable_vfs(pf_fd);
		igt_skip("VRAM BAR size is smaller than provisioned VRAM\n");
	}

	if (!resize_up && vram_bar_size <= provisioned_vram) {
		igt_sriov_disable_vfs(pf_fd);
		igt_skip("VRAM BAR size is smaller or equal to provisioned VRAM\n");
	}

	igt_assert(!intel_vram_mmap(pf_fd, vf_id, 0, vram_bar_size, PROT_READ | PROT_WRITE,
				    &vram));

	passed = validate_access_basic(&vram, vf_id, provisioned_vram);
	passed &= validate_access_beyond(&vram, vf_id, provisioned_vram, vram_bar_size);

	intel_vram_munmap(&vram);

	igt_sriov_disable_vfs(pf_fd);

	igt_assert(passed);
}

static int opts_handler(int opt, int opt_index, void *data)
{
	switch (opt) {
	case 'e':
		extended_scope = true;
		break;
	case 'v':
		verbose = true;
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

static const struct option long_opts[] = {
	{ .name = "extended", .has_arg = false, .val = 'e', },
	{ .name = "verbose", .has_arg = false, .val = 'v', },
	{}
};

static const char help_str[] =
	"  --extended\tRun the extended test scope\n"
	"  --verbose\tEnable verbose logging\n";

igt_main_args("", long_opts, help_str, opts_handler, NULL)
{
	bool autoprobe;
	int pf_fd;
	static struct subtest_resize_variants {
		const char *name;
		bool resize_up;
	} resize_variant[] = {
		{ "up", true },
		{ "down", false },
		{ NULL },
	};

	igt_fixture() {
		pf_fd = drm_open_driver(DRIVER_XE);
		igt_require(xe_has_vram(pf_fd));
		igt_require(igt_sriov_is_pf(pf_fd));
		igt_require(igt_sriov_get_enabled_vfs(pf_fd) == 0);
		autoprobe = igt_sriov_is_driver_autoprobe_enabled(pf_fd);
	}

	igt_describe("Verify that VF can access all the provisioned memory via VRAM BAR");
	igt_subtest_with_dynamic_f("vf-access-provisioned") {
		if (extended_scope) {
			for_each_sriov_num_vfs(pf_fd, num_vfs)
				igt_dynamic_f("numvfs-%d", num_vfs)
					access_provisioned(pf_fd, num_vfs);
		} else {
			for_random_sriov_num_vfs(pf_fd, num_vfs) {
				igt_dynamic_f("numvfs-random") {
					igt_debug("numvfs=%u\n", num_vfs);
					access_provisioned(pf_fd, num_vfs);
				}
			}
		}
	}

	igt_describe("Verify that VF cannot access memory beyond what's provisioned via VRAM BAR");
	igt_subtest_with_dynamic_f("vf-access-beyond") {
		if (extended_scope) {
			for_each_sriov_num_vfs(pf_fd, num_vfs)
				igt_dynamic_f("numvfs-%d", num_vfs)
					access_beyond(pf_fd, num_vfs);
		} else {
			for_random_sriov_num_vfs(pf_fd, num_vfs) {
				igt_dynamic_f("numvfs-random") {
					igt_debug("numvfs=%u\n", num_vfs);
					access_beyond(pf_fd, num_vfs);
				}
			}
		}
	}

	for (const struct subtest_resize_variants *s = resize_variant; s->name; s++) {
		igt_describe("Verify that VF can access the reprovisioned memory via VRAM BAR");
		igt_subtest_f("vf-access-after-resize-%s", s->name) {
			unsigned int total_vfs = igt_sriov_get_total_vfs(pf_fd);

			igt_require(total_vfs > 1);

			resize_and_access(pf_fd, s->resize_up);
		}
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
