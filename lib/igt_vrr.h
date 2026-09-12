/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

#ifndef IGT_VRR_H
#define IGT_VRR_H

#include <stdbool.h>
#include <stdint.h>

#include "igt.h"
#include "igt_kms.h"

extern const uint32_t igt_vrr_standard_video_timing_fps[];
extern const uint32_t igt_vrr_standard_video_timing_fps_count;

bool
igt_vrr_target_rr_debugfs_write(int fd, int crtc_index,
				uint32_t rr_numerator,
				uint32_t rr_denominator);
bool
igt_vrr_target_rr_debugfs_read(int fd, int crtc_index,
			       char *buf, size_t size);

double igt_vrr_mode_line_refresh_hz(const drmModeModeInfo *mode);

bool igt_vrr_target_refresh_rate_supported(int fd, int crtc_index);

#endif
