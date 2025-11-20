// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include <dirent.h>
#include <fcntl.h>

#include "igt.h"
#include "igt_debugfs.h"
#include "igt_dir.h"
#include "igt_sysfs.h"
#include "xe/xe_query.h"

struct {
	bool warn_on_not_hit;
} opt = { 0 };

/**
 * TEST: Xe debugfs test
 * Description: Xe-specific debugfs tests. These are complementary to the
 * core_debugfs and core_debugfs_display_on_off tests.
 *
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: uapi
 * Functionality: debugfs
 * Feature: core
 * Test category: uapi
 *
 */

IGT_TEST_DESCRIPTION("Validate Xe debugfs devnodes and their contents");

#define for_each_set_bit(__i, __mask)		\
	for (unsigned int __m = (__mask);		\
	__m && ((__i) = __builtin_ctz(__m), 1);	\
	__m &= __m - 1)

struct check_entry {
	const char *name_fmt;
	int mode;
	bool (*condition)(struct xe_device *xe_dev);
	unsigned int (*iter_mask)(struct xe_device *xe_dev);
};

static unsigned int gt_iter_mask(struct xe_device *xe_dev)
{
	return xe_dev->gt_mask;
}

static unsigned int tile_iter_mask(struct xe_device *xe_dev)
{
	return xe_dev->tile_mask;
}

static bool has_vram(struct xe_device *xe_dev)
{
	return xe_dev->has_vram;
}

/* Validate the format of debugfs fmt file, allow only one %u literal */
static bool validate_debugfs_fmt(const char *fmt)
{
	bool found_u = false;

	for (const char *p = fmt; *p; p++) {
		if (*p != '%')
			continue;

		/* % at end of string → invalid */
		if (!p[1])
			return false;

		/* must be exactly %u */
		if (p[1] != 'u')
			return false;

		/* only one %u allowed */
		if (found_u)
			return false;

		found_u = true;

		/* skip u */
		p++; /* caller loop increments p again */
	}

	return found_u;
}

struct hit_entry {
	struct igt_list_head link;
	char name[64];
};

static bool find_not_tested_files(int dir_fd, struct igt_list_head *hit_entries)
{
	igt_dir_file_list_t *file_list_entry;
	bool found_not_tested = false;
	igt_dir_t *dir = igt_dir_create(dir_fd);
	int ret;

	igt_assert_f(dir, "Failed to create igt_dir_t for debugfs dir\n");

	ret = igt_dir_scan_dirfd(dir, 0);
	igt_assert_f(ret == 0, "Failed to scan debugfs directory\n");


	igt_list_for_each_entry(file_list_entry, &dir->file_list_head, link) {
		struct hit_entry *e;
		bool hit = false;

		igt_list_for_each_entry(e, hit_entries, link) {
			if (strcmp(file_list_entry->relative_path, e->name) == 0) {
				hit = true;
				break;
			}
		}

		if (!hit) {
			igt_warn("No test for: %s\n", file_list_entry->relative_path);
			found_not_tested = true;
		}
	}

	igt_dir_destroy(dir);

	return found_not_tested;
}

static bool file_in_dir_exists(int dirfd, const char *file_name, int mode)
{
	int fd = openat(dirfd, file_name, mode);

	if (fd >= 0) {
		close(fd);
		return true;
	}

	return false;
}

/*
 * Return: negative error code on failure, or number of missing files
 */
static int debugfs_validate_entries(struct xe_device *xe_dev, int dir_fd,
				    const struct check_entry *expected_files, size_t size)
{
	struct igt_list_head hit_entries;
	int missing_count = 0;
	int err = 0;

	IGT_INIT_LIST_HEAD(&hit_entries);

	for (int i = 0; i < size; i++) {
		const struct check_entry *check = &expected_files[i];
		unsigned int mask;
		unsigned int j;

		if (check->condition && !check->condition(xe_dev))
			continue;

		if (!check->iter_mask)
			mask = BIT(0); /* to iterate once */
		else
			mask = check->iter_mask(xe_dev);

		for_each_set_bit(j, mask) {
			struct hit_entry *entry;

			entry = malloc(sizeof(*entry));
			if (!entry) {
				igt_warn("Failed to allocate memory for hit entry\n");
				err = -ENOMEM;
				goto out;
			}

			igt_list_add(&entry->link, &hit_entries);

			if (!check->iter_mask) {
				snprintf(entry->name, sizeof(entry->name), "%s", check->name_fmt);
			} else {
				int ret;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
				/**
				 * XXX: We ignore the compiler warning, but
				 * validate fmt at runtime.
				 */
				if (!validate_debugfs_fmt(check->name_fmt)) {
					igt_warn("Invalid debugfs name fmt: %s.\n",
						 check->name_fmt);
					err = -EBADF;
					goto out;
				}

				ret = snprintf(entry->name, sizeof(entry->name),
					       check->name_fmt, j);
				if (ret < 0 || ret >= sizeof(entry->name)) {
					igt_warn("Debugfs format failed for: %s\n",
						 check->name_fmt);
					err = -EINVAL;
					goto out;
				}
#pragma GCC diagnostic pop
			}

			if (!file_in_dir_exists(dir_fd, entry->name, check->mode)) {
				igt_warn("Missing debugfs file: %s\n", entry->name);
				missing_count++;
			}
		}
	}

	if (opt.warn_on_not_hit)
		find_not_tested_files(dir_fd, &hit_entries);

out:
	if (!igt_list_empty(&hit_entries)) {
		struct hit_entry *entry, *tmp;

		igt_list_for_each_entry_safe(entry, tmp, &hit_entries, link) {
			igt_list_del(&entry->link);
			free(entry);
		}
	}

	return (err < 0) ? err : missing_count;
}

/**
 * SUBTEST: root-dir
 * Description: Check required debugfs devnodes exist in the root debugfs directory
 */
static void test_root_dir(struct xe_device *xe_dev)
{
	const struct check_entry expected_files[] = {
		{ "clients", O_RDONLY },
		{ "forcewake_all", O_WRONLY },
		{ "gem_names", O_RDONLY },
		{ "gt%u", O_RDONLY, NULL, gt_iter_mask }, /* gt0, gt1, ... */
		{ "gtt_mm", O_RDONLY },
		{ "info", O_RDONLY },
		{ "name", O_RDONLY },
		{ "tile%u", O_RDONLY, NULL, tile_iter_mask }, /* tile0, tile1, ... */
	};
	int debugfs_fd = igt_debugfs_dir(xe_dev->fd);
	int missing_count;

	if (debugfs_fd < 0)
		goto skip;

	missing_count = debugfs_validate_entries(xe_dev, debugfs_fd, expected_files,
						 ARRAY_SIZE(expected_files));

	close(debugfs_fd);

	igt_fail_on_f(missing_count > 0, "Found %d missing debugfs files (see warnings above)\n",
		      missing_count);

	return;
skip:
	igt_skip("Failed to open debugfs directory\n");
}

/**
 * SUBTEST: tile-dir
 * Description: Check required debugfs devnodes exist in the tile debugfs directory
 */
static void test_tile_dir(struct xe_device *xe_dev, uint8_t tile)
{
	const struct check_entry expected_files[] = {
		{ "ggtt", O_RDONLY },
		{ "sa_info", O_RDONLY },
		{ "vram_mm", O_RDONLY, has_vram },
	};
	int debugfs_fd = igt_debugfs_tile_dir(xe_dev->fd, tile);
	int missing_count;

	igt_skip_on_f(debugfs_fd < 0, "Failed to open debugfs directory\n");

	missing_count = debugfs_validate_entries(xe_dev, debugfs_fd, expected_files,
						 ARRAY_SIZE(expected_files));

	close(debugfs_fd);

	igt_fail_on_f(missing_count > 0, "Found %d missing debugfs files (see warnings above)\n",
		      missing_count);
}

/**
 * SUBTEST: info-read
 * Description: Check info debugfs devnode contents
 */
static void test_info_read(struct xe_device *xe_dev)
{
	uint16_t devid = intel_get_drm_devid(xe_dev->fd);
	struct drm_xe_query_config *config;
	const char *name = "info";
	bool failed = false;
	char buf[4096];
	int val;

	config = xe_config(xe_dev->fd);

	igt_assert_f(config, "Failed to get xe config\n");

	snprintf(buf, sizeof(buf), "devid 0x%llx",
		 config->info[DRM_XE_QUERY_CONFIG_REV_AND_DEVICE_ID] & 0xffff);
	if (!igt_debugfs_search(xe_dev->fd, name, buf)) {
		igt_warn("Missing devid in info debugfs\n");
		failed = true;
	}

	snprintf(buf, sizeof(buf), "revid %lld",
		 config->info[DRM_XE_QUERY_CONFIG_REV_AND_DEVICE_ID] >> 16);
	if (!igt_debugfs_search(xe_dev->fd, name, buf)) {
		igt_warn("Missing revid in info debugfs\n");
		failed = true;
	}

	snprintf(buf, sizeof(buf),
		 "is_dgfx %s", config->info[DRM_XE_QUERY_CONFIG_FLAGS] &
		DRM_XE_QUERY_CONFIG_FLAG_HAS_VRAM ? "yes" : "no");
	if (!igt_debugfs_search(xe_dev->fd, name, buf)) {
		igt_warn("Missing is_dgfx in info debugfs\n");
		failed = true;
	}

	if (intel_gen(devid) < 20) {
		val = -1;

		switch (config->info[DRM_XE_QUERY_CONFIG_VA_BITS]) {
		case 48:
			val = 3;
			break;
		case 57:
			val = 4;
			break;
		default:
			igt_warn("Unexpected va_bits value: %lld\n",
				 config->info[DRM_XE_QUERY_CONFIG_VA_BITS]);
			failed = true;
			break;
		}

		if (val != -1) {
			snprintf(buf, sizeof(buf), "vm_max_level %d", val);
			if (!igt_debugfs_search(xe_dev->fd, name, buf)) {
				igt_warn("Missing vm_max_level in info debugfs\n");
				failed = true;
			}
		}
	}

	snprintf(buf, sizeof(buf), "tile_count %d", xe_sysfs_get_num_tiles(xe_dev->fd));
	if (!igt_debugfs_search(xe_dev->fd, name, buf)) {
		igt_warn("Missing tile_count in info debugfs\n");
		failed = true;
	}

	igt_fail_on_f(failed, "Some required info debugfs entries are missing\n");

}

const char *help_str =
	"  --warn-not-hit|--w\tWarn about devfs nodes that have no tests";

struct option long_options[] = {
	{ "warn-not-hit", no_argument, NULL, 'w'},
	{ }
};

static int opt_handler(int option, int option_index, void *input)
{
	switch (option) {
	case 'w':
		opt.warn_on_not_hit = true;
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

igt_main_args("", long_options, help_str, opt_handler, NULL)
{
	struct xe_device *xe_dev;
	unsigned int t;
	int fd = -1;

	igt_fixture() {
		fd = drm_open_driver_master(DRIVER_XE);
		xe_dev = xe_device_get(fd);
		igt_assert_f(xe_dev, "Failed to get xe device\n");
		kmstest_set_vt_graphics_mode();
	}

	igt_describe("Check required debugfs devnodes exist in the root debugfs directory.");
	igt_subtest("root-dir")
		test_root_dir(xe_dev);

	igt_describe("Check required debugfs devnodes exist in the tile debugfs directory.");
	igt_subtest_with_dynamic("tile-dir")
			xe_for_each_tile(fd, t)
				igt_dynamic_f("tile-%u", t)
					test_tile_dir(xe_dev, t);

	igt_describe("Check info debugfs devnode contents.");
	igt_subtest("info-read")
		test_info_read(xe_dev);
	igt_fixture() {
		drm_close_driver(fd);
	}

}
