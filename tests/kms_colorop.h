/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2025 Advanced Micro Devices, Inc.
 */

#ifndef __KMS_COLOROP_H__
#define __KMS_COLOROP_H__

#include "igt_color.h"

typedef bool (*compare_fb_t)(igt_fb_t *in, igt_fb_t *out);

typedef int (*transform_fb)(igt_fb_t *in);

typedef int (*transform_pixel)(igt_pixel_t *pixel);

/* Test version definitions */
typedef enum kms_colorop_type {
	KMS_COLOROP_ENUMERATED_LUT1D,
	KMS_COLOROP_CUSTOM_LUT1D,
	KMS_COLOROP_CTM,
	KMS_COLOROP_LUT3D
} kms_colorop_type_t;

typedef enum kms_colorop_lut1d_tf {
	KMS_COLOROP_LUT1D_SRGB_EOTF,
	KMS_COLOROP_LUT1D_SRGB_INV_EOTF,
	KMS_COLOROP_LUT1D_PQ_EOTF,
	KMS_COLOROP_LUT1D_PQ_INV_EOTF,
} kms_colorop_lut1d_tf_t;

typedef struct kms_colorop_enumerated_lut1d_info {
	kms_colorop_lut1d_tf_t tf;
} kms_colorop_enumerated_lut1d_info_t;

typedef struct kms_colorop {
	kms_colorop_type_t type;

	union {
		kms_colorop_enumerated_lut1d_info_t enumerated_lut1d_info;
	};

	const char *name;

	igt_pixel_transform transform;

	/* Mapped colorop */
	igt_colorop_t *colorop;

} kms_colorop_t;

kms_colorop_t kms_colorop_srgb_eotf = {
	.type = KMS_COLOROP_ENUMERATED_LUT1D,
	.enumerated_lut1d_info = {
		.tf = KMS_COLOROP_LUT1D_SRGB_EOTF
	},
	.name = "srgb_eotf",
	.transform = &igt_color_srgb_eotf
};

kms_colorop_t kms_colorop_srgb_inv_eotf = {
	.type = KMS_COLOROP_ENUMERATED_LUT1D,
	.enumerated_lut1d_info = {
		.tf = KMS_COLOROP_LUT1D_SRGB_INV_EOTF
	},
	.name = "srgb_inv_eotf",
	.transform = &igt_color_srgb_inv_eotf
};

#endif /* __KMS_COLOROP_H__ */
