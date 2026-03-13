/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef INTEL_FBC_H
#define INTEL_FBC_H

#include "igt.h"

enum psr_mode;

void intel_fbc_enable(igt_display_t *display);
void intel_fbc_disable(igt_display_t *display);
bool intel_fbc_supported(igt_crtc_t *crtc);
bool intel_fbc_wait_until_enabled(igt_crtc_t *crtc);
bool intel_fbc_is_enabled(igt_crtc_t *crtc, int log_level);
bool intel_fbc_plane_size_supported(igt_display_t *display, uint32_t width, uint32_t height);
bool intel_fbc_supported_for_psr_mode(int disp_ver, enum psr_mode mode);

#endif
