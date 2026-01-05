/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2023 Advanced Micro Devices, Inc.
 *
 * This file contains code adapted from Skia, which is
 * distributed under a BSD-style license which can be
 * found at
 * https://skia.googlesource.com/skia.git/+/refs/heads/main/LICENSE
 */


#ifndef __IGT_COLOR_H__
#define __IGT_COLOR_H__

#include <limits.h>

#include "igt_fb.h"
#include "igt_kms.h"
#include "igt_color_lut.h"

#define MAX_COLOR_LUT_ENTRIES 4096

struct igt_color_tf {
	float g, a, b, c, d, e, f;
};

struct igt_color_tf_pq {
	float A, B, C, D, E, F, G;
};

extern const struct igt_color_tf srgb_eotf;
extern const struct igt_color_tf bt2020_inv_oetf;
extern const struct igt_color_tf_pq pq_eotf;

typedef struct igt_pixel {
	float r;
	float g;
	float b;
} igt_pixel_t;

typedef struct igt_1dlut {
	struct drm_color_lut32 lut[MAX_COLOR_LUT_ENTRIES];
} igt_1dlut_t;

extern igt_1dlut_t igt_1dlut_srgb_inv_eotf;
extern igt_1dlut_t igt_1dlut_srgb_eotf;

extern igt_1dlut_t igt_1dlut_linear;
extern igt_1dlut_t igt_1dlut_max;

typedef struct igt_matrix_3x4 {
	/*
	 * out   matrix          in
	 * |R|   |0  1  2  3 |   | R |
	 * |G| = |4  5  6  7 | x | G |
	 * |B|   |8  9  10 11|   | B |
	 *                       |1.0|
	 */
	float m[12];
} igt_matrix_3x4_t;

extern const igt_matrix_3x4_t igt_matrix_3x4_50_desat;
extern const igt_matrix_3x4_t igt_matrix_3x4_overdrive;
extern const igt_matrix_3x4_t igt_matrix_3x4_oversaturate;
extern const igt_matrix_3x4_t igt_matrix_3x4_bt709_enc;
extern const igt_matrix_3x4_t igt_matrix_3x4_bt709_dec;

bool igt_cmp_fb_component(uint16_t comp1, uint16_t comp2, uint8_t up, uint8_t down);
bool igt_cmp_fb_pixels(igt_fb_t *fb1, igt_fb_t *fb2, uint8_t up, uint8_t down);

void igt_dump_fb(igt_display_t *display, igt_fb_t *fb, const char *path_name, const char *file_name);

typedef void (*igt_pixel_transform)(igt_pixel_t *pixel);

int igt_color_transform_pixels(igt_fb_t *fb, igt_pixel_transform transforms[], int num_transforms);

/* colorop helpers */

void igt_colorop_set_ctm_3x4(igt_display_t *display,
			     igt_colorop_t *colorop,
			     const igt_matrix_3x4_t *matrix);

void igt_colorop_set_custom_1dlut(igt_display_t *display,
				  igt_colorop_t *colorop,
				  const igt_1dlut_t *lut1d,
				  const size_t lut_size);

void igt_colorop_set_3dlut(igt_display_t *display,
			   igt_colorop_t *colorop,
			   const igt_3dlut_norm_t *lut3d,
			   const size_t lut_size);


/* transformations */

void igt_color_srgb_inv_eotf(igt_pixel_t *pixel);
void igt_color_srgb_eotf(igt_pixel_t *pixel);

void igt_color_max(igt_pixel_t *pixel);
void igt_color_linear(igt_pixel_t *pixel);

void igt_color_pq_inv_eotf(igt_pixel_t *pixel);
void igt_color_pq_eotf(igt_pixel_t *pixel);

void igt_color_pq_125_inv_eotf(igt_pixel_t *pixel);
void igt_color_pq_125_eotf(igt_pixel_t *pixel);

void igt_color_bt2020_inv_oetf(igt_pixel_t *pixel);
void igt_color_bt2020_oetf(igt_pixel_t *pixel);

void igt_color_gamma_2_2_oetf(igt_pixel_t *pixel);
void igt_color_gamma_2_2_inv_oetf(igt_pixel_t *pixel);

void igt_color_ctm_3x4_50_desat(igt_pixel_t *pixel);
void igt_color_ctm_3x4_overdrive(igt_pixel_t *pixel);
void igt_color_ctm_3x4_oversaturate(igt_pixel_t *pixel);
void igt_color_ctm_3x4_bt709_dec(igt_pixel_t *pixel);
void igt_color_ctm_3x4_bt709_enc(igt_pixel_t *pixel);

void igt_color_multiply_125(igt_pixel_t *pixel);
void igt_color_multiply_inv_125(igt_pixel_t *pixel);
void igt_color_3dlut_17_12_rgb(igt_pixel_t *pixel);
void igt_color_3dlut_17_12_bgr(igt_pixel_t *pixel);

#endif
