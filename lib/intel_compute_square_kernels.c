/* SPDX-License-Identifier: MIT */

/*
 * Copyright © 2022 Intel Corporation
 *
 * Authors:
 *		Francois Dugast <francois.dugast@intel.com>
 */

#define INTEL_COMPUTE_KRN_COMPILE_GUARD 1

#include "intel_chipset.h"
#include "lib/intel_compute.h"
#include "lib/intel_compute_krn_sip.h"
#include "lib/intel_compute_krn_square.h"
#include "lib/intel_compute_krn_loop_count.h"
#include "lib/intel_compute_krn_loop.h"

const struct intel_compute_kernels intel_compute_square_kernels[] = {
	{
		.ip_ver = IP_VER(12, 0),
		.size = sizeof(tgllp_kernel_square_bin),
		.kernel = tgllp_kernel_square_bin,
	},
	{
		.ip_ver = IP_VER(12, 10),
		.size = sizeof(dg1_kernel_square_bin),
		.kernel = dg1_kernel_square_bin,
	},
	{
		.ip_ver = IP_VER(12, 55),
		.size = sizeof(xehp_kernel_square_bin),
		.kernel = xehp_kernel_square_bin,
	},
	{
		.ip_ver = IP_VER(12, 60),
		.size = sizeof(xehpc_kernel_square_bin),
		.kernel = xehpc_kernel_square_bin,
	},
	{
		.ip_ver = IP_VER(12, 70),
		.size = sizeof(xelpg_kernel_square_bin),
		.kernel = xelpg_kernel_square_bin,
	},
	{
		.ip_ver = IP_VER(20, 01),
		.size = sizeof(xe2lpg_kernel_square_bin),
		.kernel = xe2lpg_kernel_square_bin,
		.long_kernel = xe2lpg_kernel_count_bin,
		.long_kernel_size = sizeof(xe2lpg_kernel_count_bin),
		.sip_kernel = xe2lpg_kernel_sip_bin,
		.sip_kernel_size = sizeof(xe2lpg_kernel_sip_bin),
		.loop_kernel = xe2lpg_kernel_loop_bin,
		.loop_kernel_size = sizeof(xe2lpg_kernel_loop_bin),
	},
	{
		.ip_ver = IP_VER(20, 04),
		.size = sizeof(xe2lpg_kernel_square_bin),
		.kernel = xe2lpg_kernel_square_bin,
		.long_kernel = xe2lpg_kernel_count_bin,
		.long_kernel_size = sizeof(xe2lpg_kernel_count_bin),
		.sip_kernel = xe2lpg_kernel_sip_bin,
		.sip_kernel_size = sizeof(xe2lpg_kernel_sip_bin),
		.loop_kernel = xe2lpg_kernel_loop_bin,
		.loop_kernel_size = sizeof(xe2lpg_kernel_loop_bin),
	},
	{
		.ip_ver = IP_VER(30, 00),
		.size = sizeof(xe3lpg_kernel_square_bin),
		.kernel = xe3lpg_kernel_square_bin,
		.long_kernel = xe3lpg_kernel_count_bin,
		.long_kernel_size = sizeof(xe3lpg_kernel_count_bin),
		.sip_kernel = xe3lpg_kernel_sip_bin,
		.sip_kernel_size = sizeof(xe3lpg_kernel_sip_bin),
		.loop_kernel = xe3lpg_kernel_loop_bin,
		.loop_kernel_size = sizeof(xe3lpg_kernel_loop_bin),
	},
	{
		.ip_ver = IP_VER(30, 04),
		.size = sizeof(xe3lpg_kernel_square_bin),
		.kernel = xe3lpg_kernel_square_bin,
		.long_kernel = xe3lpg_kernel_count_bin,
		.long_kernel_size = sizeof(xe3lpg_kernel_count_bin),
		.sip_kernel = xe3lpg_kernel_sip_bin,
		.sip_kernel_size = sizeof(xe3lpg_kernel_sip_bin),
		.loop_kernel = xe3lpg_kernel_loop_bin,
		.loop_kernel_size = sizeof(xe3lpg_kernel_loop_bin),
	},
	{
		.ip_ver = IP_VER(35, 11),
		.size = sizeof(xe3p_kernel_square_bin),
		.kernel = xe3p_kernel_square_bin,
		.long_kernel = xe3p_kernel_count_bin,
		.long_kernel_size = sizeof(xe3p_kernel_count_bin),
		.loop_kernel = xe3p_kernel_loop_bin,
		.loop_kernel_size = sizeof(xe3p_kernel_loop_bin),
	},
	{}
};
