/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef KMS_JOINER_HELPER_H
#define KMS_JOINER_HELPER_H

#include "igt_kms.h"

void igt_set_all_master_pipes_for_platform(igt_display_t *display,
					   uint32_t *master_pipes);
bool igt_assign_pipes_for_outputs(int drm_fd,
				  igt_output_t **outputs,
				  int num_outputs,
				  int n_pipes,
				  uint32_t *used_pipes_mask,
				  uint32_t master_pipes_mask,
				  uint32_t valid_pipes_mask);

enum force_joiner_mode {
	FORCE_JOINER_ENABLE = 0,
	FORCE_JOINER_DISABLE
};
#endif
