/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef INTEL_PAT_H
#define INTEL_PAT_H

#include <stdbool.h>
#include <stdint.h>

#define DEFAULT_PAT_INDEX ((uint8_t)-1) /* igt-core can pick 1way or better */
#define XE_PAT_IDX_INVALID ((uint8_t)-2) /* no such PAT index on this platform */
#define XE_PAT_MAX_ENTRIES 32

struct xe_pat_entry {
	uint32_t pat;
	bool rsvd;
};

struct intel_pat_cache {
	uint8_t uc; /* UC + COH_NONE */
	uint8_t wt; /* WT + COH_NONE */
	uint8_t wb; /* WB + COH_AT_LEAST_1WAY */
	uint8_t uc_comp; /* UC + COH_NONE + COMPRESSION, XE2 and later*/
	uint8_t max_index;
	struct xe_pat_entry entries[XE_PAT_MAX_ENTRIES];
	uint32_t pta_mode;
	uint32_t pat_ats;
};

uint8_t intel_get_max_pat_index(int fd);

uint8_t intel_get_pat_idx_uc(int fd);
uint8_t intel_get_pat_idx_wt(int fd);
uint8_t intel_get_pat_idx_wb(int fd);

uint8_t intel_get_pat_idx_uc_comp(int fd);

int32_t xe_get_pat_sw_config(int drm_fd, struct intel_pat_cache *xe_pat_cache, int gt);
int32_t xe_get_pat_hw_config(int drm_fd, struct intel_pat_cache *xe_pat_cache, int gt);

#endif /* INTEL_PAT_H */
