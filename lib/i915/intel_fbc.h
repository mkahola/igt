/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef INTEL_FBC_H
#define INTEL_FBC_H

#include "igt.h"

#define intel_fbc_enable(device) igt_set_module_param_int(device, "enable_fbc", 1)
#define intel_fbc_disable(device) igt_set_module_param_int(device, "enable_fbc", 0)

enum psr_mode;

bool intel_fbc_supported(igt_crtc_t *crtc);
bool intel_fbc_wait_until_enabled(igt_crtc_t *crtc);
bool intel_fbc_is_enabled(igt_crtc_t *crtc, int log_level);
bool intel_fbc_plane_size_supported(igt_display_t *display, uint32_t width, uint32_t height);
bool intel_fbc_supported_for_psr_mode(int disp_ver, enum psr_mode mode);

#endif
