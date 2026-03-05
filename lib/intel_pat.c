// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include <fcntl.h>
#include "igt.h"
#include "intel_pat.h"
#include "xe/xe_query.h"

/**
 * xe_get_pat_sw_config - Helper to read PAT (Page Attribute Table) software configuration
 * from debugfs
 *
 * @drm_fd: DRM device fd to use with igt_debugfs_open
 * @xe_pat_cache: Pointer to a struct that will receive the parsed PAT configuration
 *
 * Returns: The number of PAT entries successfully read on success, or a negative error
 *          code on failure
 */
int32_t xe_get_pat_sw_config(int drm_fd, struct intel_pat_cache *xe_pat_cache)
{
	char *line = NULL;
	size_t line_len = 0;
	ssize_t nread;
	int32_t parsed = 0;
	int dbgfs_fd;
	FILE *dbgfs_file = NULL;

	dbgfs_fd = igt_debugfs_open(drm_fd, "gt0/pat_sw_config", O_RDONLY);
	if (dbgfs_fd < 0)
		return dbgfs_fd;
	dbgfs_file = fdopen(dbgfs_fd, "r");
	if (!dbgfs_file) {
		close(dbgfs_fd);
		return -errno;
	}

	memset(xe_pat_cache, 0, sizeof(*xe_pat_cache));
	while ((nread = getline(&line, &line_len, dbgfs_file)) != -1) {
		uint32_t value = 0;
		char *p = NULL;

		/* Expect patterns like: PAT[ 0] = [ 0, 0, 0, 0, 0 ]  (     0x0) */
		if (strncmp(line, "PAT[", 4) == 0) {
			bool is_reserved;
			p = strstr(line, "(");
			if (p && sscanf(p, "(%x", &value) == 1) {
				if (parsed < XE_PAT_MAX_ENTRIES) {
					is_reserved = strchr(line, '*') != NULL;
					xe_pat_cache->entries[parsed].pat = value;
					xe_pat_cache->entries[parsed].rsvd = is_reserved;
					xe_pat_cache->max_index = parsed;
					parsed++;

					igt_debug("Parsed PAT entry %d: 0x%08x%s\n",
						  parsed - 1, value,
						  is_reserved ? " (reserved)" : "");
				} else {
					igt_warn("Too many PAT entries, line ignored: %s\n", line);
				}
			} else {
				igt_warn("Failed to parse PAT entry line: %s\n", line);
			}
		} else if (strncmp(line, "PAT_MODE", 8) == 0) {
			p = strstr(line, "(");
			if (p && sscanf(p, "(%x", &value) == 1)
				xe_pat_cache->pat_mode = value;
		} else if (strncmp(line, "PAT_ATS", 7) == 0) {
			p = strstr(line, "(");
			if (p && sscanf(p, "(%x", &value) == 1)
				xe_pat_cache->pat_ats = value;
		} else if (strncmp(line, "IDX[XE_CACHE_NONE]", 18) == 0) {
			p = strstr(line, "=");
			if (p && sscanf(p, "= %d", &value) == 1)
				xe_pat_cache->uc = value;
		} else if (strncmp(line, "IDX[XE_CACHE_WT]", 16) == 0) {
			p = strstr(line, "=");
			if (p && sscanf(p, "= %d", &value) == 1)
				xe_pat_cache->wt = value;
		} else if (strncmp(line, "IDX[XE_CACHE_WB]", 16) == 0) {
			p = strstr(line, "=");
			if (p && sscanf(p, "= %d", &value) == 1)
				xe_pat_cache->wb = value;
		} else if (strncmp(line, "IDX[XE_CACHE_NONE_COMPRESSION]", 28) == 0) {
			p = strstr(line, "=");
			if (p && sscanf(p, "= %d", &value) == 1)
				xe_pat_cache->uc_comp = value;
		}
	}

	free(line);
	fclose(dbgfs_file);

	return parsed;
}

static void intel_get_pat_idx(int fd, struct intel_pat_cache *pat)
{
	uint16_t dev_id;

	/*
	 * For Xe, use the PAT cache stored in struct xe_device.
	 * xe_device_get() populates the cache while still root; forked
	 * children that inherit the xe_device can use it post-drop_root().
	 */
	if (is_xe_device(fd)) {
		struct xe_device *xe_dev = xe_device_get(fd);

		igt_assert_f(xe_dev->pat_cache,
			     "PAT sw_config not available -- "
			     "debugfs not accessible (missing root or not mounted?)\n");
		*pat = *xe_dev->pat_cache;
		return;
	}

	/* i915 fallback: hardcoded PAT indices */
	dev_id = intel_get_drm_devid(fd);

	if (IS_METEORLAKE(dev_id)) {
		pat->uc = 2;
		pat->wt = 1;
		pat->wb = 3;
		pat->max_index = 3;
	} else if (IS_PONTEVECCHIO(dev_id)) {
		pat->uc = 0;
		pat->wt = 2;
		pat->wb = 3;
		pat->max_index = 7;
	} else if (intel_graphics_ver(dev_id) <= IP_VER(12, 60)) {
		pat->uc = 3;
		pat->wt = 2;
		pat->wb = 0;
		pat->max_index = 3;
	} else {
		igt_critical("Platform is missing PAT settings for uc/wt/wb\n");
	}
}

uint8_t intel_get_max_pat_index(int fd)
{
	struct intel_pat_cache pat = {};

	intel_get_pat_idx(fd, &pat);
	return pat.max_index;
}

uint8_t intel_get_pat_idx_uc(int fd)
{
	struct intel_pat_cache pat = {};

	intel_get_pat_idx(fd, &pat);
	return pat.uc;
}

uint8_t intel_get_pat_idx_uc_comp(int fd)
{
	struct intel_pat_cache pat = {};
	uint16_t dev_id = intel_get_drm_devid(fd);

	igt_assert(intel_gen(dev_id) >= 20);

	intel_get_pat_idx(fd, &pat);
	return pat.uc_comp;
}

uint8_t intel_get_pat_idx_wt(int fd)
{
	struct intel_pat_cache pat = {};

	intel_get_pat_idx(fd, &pat);
	return pat.wt;
}

uint8_t intel_get_pat_idx_wb(int fd)
{
	struct intel_pat_cache pat = {};

	intel_get_pat_idx(fd, &pat);
	return pat.wb;
}
