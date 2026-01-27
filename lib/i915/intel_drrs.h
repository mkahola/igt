/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef INTEL_DRRS_H
#define INTEL_DRRS_H

#include "igt.h"

bool intel_is_drrs_supported(int device, int crtc_index);
bool intel_output_has_drrs(int device, igt_output_t *output);
void intel_drrs_enable(int device, int crtc_index);
void intel_drrs_disable(int device, int crtc_index);
bool intel_is_drrs_inactive(int device, int crtc_index);

#endif
