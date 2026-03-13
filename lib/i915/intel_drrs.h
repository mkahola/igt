/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef INTEL_DRRS_H
#define INTEL_DRRS_H

#include "igt.h"

bool intel_is_drrs_supported(igt_crtc_t *crtc);
bool intel_output_has_drrs(int device, igt_output_t *output);
void intel_drrs_enable(igt_crtc_t *crtc);
void intel_drrs_disable(igt_crtc_t *crtc);
bool intel_is_drrs_inactive(igt_crtc_t *crtc);

void intel_drrs_disable_crtc_index(int device, int crtc_index);

#endif
