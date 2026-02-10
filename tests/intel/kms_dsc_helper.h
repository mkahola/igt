/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef IGT_KMS_DSC_HELPER_H
#define IGT_KMS_DSC_HELPER_H

#include "igt.h"
#include "igt_sysfs.h"
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <strings.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <termios.h>

void force_dsc_enable(int drmfd, igt_output_t *output);
void force_dsc_enable_bpc(int drmfd, igt_output_t *output, int input_bpc);
void save_force_dsc_en(int drmfd, igt_output_t *output);
void restore_force_dsc_en(void);
void kms_dsc_exit_handler(int sig);
bool is_dsc_supported_by_sink(int drmfd, igt_output_t *output);
bool is_dsc_supported_by_source(int drmfd);
bool check_gen11_dp_constraint(int drmfd, igt_output_t *output,
			       igt_crtc_t *crtc);
bool check_gen11_bpc_constraint(int drmfd, int input_bpc);
void force_dsc_output_format(int drmfd, igt_output_t *output,
			     enum dsc_output_format output_format);
bool is_dsc_output_format_supported(int disp_ver, int drmfd, igt_output_t *output,
				    enum dsc_output_format output_format);
void force_dsc_fractional_bpp_enable(int drmfd, igt_output_t *output);
void save_force_dsc_fractional_bpp_en(int drmfd, igt_output_t *output);
void restore_force_dsc_fractional_bpp_en(void);
bool is_dsc_fractional_bpp_supported(int disp_ver, int drmfd, igt_output_t *output);

#endif
