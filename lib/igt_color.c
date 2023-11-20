// SPDX-License-Identifier: MIT
/*
 * Copyright 2023 Advanced Micro Devices, Inc.
 *
 * This file contains code adapted from Skia, which is
 * distributed under a BSD-style license which can be
 * found at
 * https://skia.googlesource.com/skia.git/+/refs/heads/main/LICENSE
 */

#include <errno.h>
#include <math.h>

#include "drmtest.h"
#include "igt_color.h"
#include "igt_core.h"
#include "igt_x86.h"


static float clamp(float val, float min, float max)
{
	return ((val < min) ? min : ((val > max) ? max : val));
}

static float igt_color_tf_eval_unclamped(const struct igt_color_tf *fn, float x)
{
	if (x < fn->d)
		return fn->c * x + fn->f;

	return pow(fn->a * x + fn->b, fn->g) + fn->e;
}

static float igt_color_tf_eval(const struct igt_color_tf *fn, float x)
{
	float fn_at_x_unclamped = igt_color_tf_eval_unclamped(fn, x);

	return clamp(fn_at_x_unclamped, 0.0f, 1.0f);
}

static void tf_inverse(const struct igt_color_tf *fn, struct igt_color_tf *inv)
{
	memset(inv, 0, sizeof(struct igt_color_tf));

	if (fn->a > 0 && fn->g > 0) {
		double a_to_the_g = pow(fn->a, fn->g);

		inv->a = 1.f / a_to_the_g;
		inv->b = -fn->e / a_to_the_g;
		inv->g = 1.f / fn->g;
	}

	inv->d = fn->c * fn->d + fn->f;
	inv->e = -fn->b / fn->a;
	if (fn->c != 0) {
		inv->c = 1.f / fn->c;
		inv->f = -fn->f / fn->c;
	}
}

void igt_color_srgb_inv_eotf(igt_pixel_t *pixel)
{
	struct igt_color_tf inv;

	tf_inverse(&srgb_eotf, &inv);

	pixel->r = igt_color_tf_eval(&inv, pixel->r);
	pixel->g = igt_color_tf_eval(&inv, pixel->g);
	pixel->b = igt_color_tf_eval(&inv, pixel->b);
}



void igt_color_srgb_eotf(igt_pixel_t *pixel)
{
	pixel->r = igt_color_tf_eval(&srgb_eotf, pixel->r);
	pixel->g = igt_color_tf_eval(&srgb_eotf, pixel->g);
	pixel->b = igt_color_tf_eval(&srgb_eotf, pixel->b);
}

static void igt_color_apply_3x4_ctm(igt_pixel_t *pixel, const igt_matrix_3x4_t *matrix)
{
	igt_pixel_t result;

	memcpy(&result, pixel, sizeof(result));

	result.r = matrix->m[0] * pixel->r +
		   matrix->m[1] * pixel->g +
		   matrix->m[2] * pixel->b +
		   matrix->m[3];

	result.g = matrix->m[4] * pixel->r +
		   matrix->m[5] * pixel->g +
		   matrix->m[6] * pixel->b +
		   matrix->m[7];

	result.b = matrix->m[8] * pixel->r +
		   matrix->m[9] * pixel->g +
		   matrix->m[10] * pixel->b +
		   matrix->m[11];

	memcpy(pixel, &result, sizeof(result));

}

void igt_color_ctm_3x4_50_desat(igt_pixel_t *pixel)
{
	/* apply a 50% desat matrix */
	igt_color_apply_3x4_ctm(pixel, &igt_matrix_3x4_50_desat);
}

void igt_color_ctm_3x4_overdrive(igt_pixel_t *pixel)
{
	/* apply a 50% desat matrix */
	igt_color_apply_3x4_ctm(pixel, &igt_matrix_3x4_overdrive);
}

void igt_color_ctm_3x4_oversaturate(igt_pixel_t *pixel)
{
	/* apply a 50% desat matrix */
	igt_color_apply_3x4_ctm(pixel, &igt_matrix_3x4_oversaturate);
}

void igt_color_ctm_3x4_bt709_enc(igt_pixel_t *pixel)
{
	/* apply a 50% desat matrix */
	igt_color_apply_3x4_ctm(pixel, &igt_matrix_3x4_bt709_enc);
}

void igt_color_ctm_3x4_bt709_dec(igt_pixel_t *pixel)
{
	/* apply a 50% desat matrix */
	igt_color_apply_3x4_ctm(pixel, &igt_matrix_3x4_bt709_dec);
}

static void
igt_color_fourcc_to_pixel(uint32_t raw_pixel, uint32_t drm_format, igt_pixel_t *pixel)
{
	if (drm_format == DRM_FORMAT_XRGB8888) {
		raw_pixel &= 0x00ffffff;

		pixel->r = (raw_pixel & 0x00ff0000) >> 16;
		pixel->g = (raw_pixel & 0x0000ff00) >> 8;
		pixel->b = (raw_pixel & 0x000000ff);

		/* normalize for 8-bit */
		pixel->r /= (0xff);
		pixel->g /= (0xff);
		pixel->b /= (0xff);
	} else if (drm_format == DRM_FORMAT_XRGB2101010) {
		raw_pixel &= 0x3fffffff;

		pixel->r = (raw_pixel & 0x3ff00000) >> 20;
		pixel->g = (raw_pixel & 0x000ffc00) >> 10;
		pixel->b = (raw_pixel & 0x000003ff);

		/* normalize for 10-bit */
		pixel->r /= (0x3ff);
		pixel->g /= (0x3ff);
		pixel->b /= (0x3ff);
	} else {
		igt_skip("pixel format support not implemented");
	}
}

static uint32_t
igt_color_pixel_to_fourcc(uint32_t drm_format, igt_pixel_t *pixel)
{
	uint32_t raw_pixel;

	/* clip */
	pixel->r = fmax(fmin(pixel->r, 1.0), 0.0);
	pixel->g = fmax(fmin(pixel->g, 1.0), 0.0);
	pixel->b = fmax(fmin(pixel->b, 1.0), 0.0);

	if (drm_format == DRM_FORMAT_XRGB8888) {
		/* de-normalize back to 8-bit */
		pixel->r *= (0xff);
		pixel->g *= (0xff);
		pixel->b *= (0xff);

		/* re-pack pixel into FB*/
		raw_pixel = 0x0;
		raw_pixel |= ((uint8_t)(lround(pixel->r) & 0xff)) << 16;
		raw_pixel |= ((uint8_t)(lround(pixel->g) & 0xff)) << 8;
		raw_pixel |= ((uint8_t)(lround(pixel->b) & 0xff));
	} else if (drm_format == DRM_FORMAT_XRGB2101010) {
		/* de-normalize back to 10-bit */
		pixel->r *= (0x3ff);
		pixel->g *= (0x3ff);
		pixel->b *= (0x3ff);

		/* re-pack pixel into FB*/
		raw_pixel = 0x0;
		raw_pixel |= (lround(pixel->r) & 0x3ff) << 20;
		raw_pixel |= (lround(pixel->g) & 0x3ff) << 10;
		raw_pixel |= (lround(pixel->b) & 0x3ff);
	} else {
		igt_skip("pixel format support not implemented");
	}

	return raw_pixel;
}

int igt_color_transform_pixels(igt_fb_t *fb, igt_pixel_transform transforms[], int num_transforms)
{
	uint32_t *line = NULL;
	void *map;
	char *ptr;
	int x, y, cpp = igt_drm_format_to_bpp(fb->drm_format) / 8;
	uint32_t stride = igt_fb_calc_plane_stride(fb, 0);

	if (fb->num_planes != 1)
		return -EINVAL;

	ptr = igt_fb_map_buffer(fb->fd, fb);
	igt_assert(ptr);
	map = ptr;

	/*
	 * Framebuffers are often uncached, which can make byte-wise accesses
	 * very slow. We copy each line of the FB into a local buffer to speed
	 * up the hashing.
	 */
	line = malloc(stride);
	if (!line) {
		munmap(map, fb->size);
		return -ENOMEM;
	}

	for (y = 0; y < fb->height; y++, ptr += stride) {

		/* get line from buffer */
		igt_memcpy_from_wc(line, ptr, fb->width * cpp);

		for (x = 0; x < fb->width; x++) {
			uint32_t raw_pixel = le32_to_cpu(line[x]);
			igt_pixel_t pixel;
			int i;

			igt_color_fourcc_to_pixel(raw_pixel, fb->drm_format, &pixel);

			/* run transform on pixel */
			for (i = 0; i < num_transforms; i++)
				transforms[i](&pixel);

			/* write back to line */
			line[x] = cpu_to_le32(igt_color_pixel_to_fourcc(fb->drm_format, &pixel));
		}

		/* copy line back to fb buffer */
		igt_memcpy_from_wc(ptr, line, fb->width * cpp);
	}

	free(line);
	igt_fb_unmap_buffer(fb, map);

	return 0;
}

bool igt_cmp_fb_component(uint16_t comp1, uint16_t comp2, uint8_t up, uint8_t down)
{
	int16_t diff = comp2 - comp1;

	if (diff < -down || diff > up)
		return false;

	return true;
}

bool igt_cmp_fb_pixels(igt_fb_t *fb1, igt_fb_t *fb2, uint8_t up, uint8_t down)
{
	uint32_t *ptr1, *ptr2;
	uint32_t pixel1, pixel2, i, j;
	bool matched = true;

	ptr1 = igt_fb_map_buffer(fb1->fd, fb1);
	ptr2 = igt_fb_map_buffer(fb2->fd, fb2);

	igt_assert(fb1->drm_format == fb2->drm_format);
	igt_assert(fb1->size == fb2->size);

	for (i = 0; i < fb1->size / sizeof(uint32_t); i++) {
		uint16_t mask = 0xff;
		uint16_t shift = 8;

		if (fb1->drm_format == DRM_FORMAT_XRGB2101010) {
			/* ignore alpha */
			pixel1 = ptr1[i] & ~0xc0000000;
			pixel2 = ptr2[i] & ~0xc0000000;

			mask = 0x3ff;
			shift = 10;
		} else if (fb1->drm_format == DRM_FORMAT_XRGB8888) {
			/* ignore alpha */
			pixel1 = ptr1[i] & ~0xff000000;
			pixel2 = ptr2[i] & ~0xff000000;

			mask = 0xff;
			shift = 8;
		} else {
			pixel1 = ptr1[i];
			pixel2 = ptr2[i];
		}

		for (j = 0; j < 3; j++) {
			uint16_t comp1 = (pixel1 >> (shift * j)) & mask;
			uint16_t comp2 = (pixel2 >> (shift * j)) & mask;

			if (!igt_cmp_fb_component(comp1, comp2, up, down)) {
				igt_info("i %d j %d shift %d mask %x comp1 %x comp2 %x, pixel1 %x pixel2 %x\n",
					 i, j, shift, mask, comp1, comp2, pixel1, pixel2);
				return false;
			}
		}
	}

	igt_fb_unmap_buffer(fb1, ptr1);
	igt_fb_unmap_buffer(fb2, ptr2);

	return matched;
}

void igt_dump_fb(igt_display_t *display, igt_fb_t *fb,
		 const char *path_name, const char *file_name)
{
	char filepath_out[PATH_MAX];
	cairo_surface_t *fb_surface_out;
	cairo_status_t status;

	snprintf(filepath_out, PATH_MAX, "%s/%s.png", path_name, file_name);
	fb_surface_out = igt_get_cairo_surface(display->drm_fd, fb);
	status = cairo_surface_write_to_png(fb_surface_out, filepath_out);
	igt_assert_eq(status, CAIRO_STATUS_SUCCESS);
	cairo_surface_destroy(fb_surface_out);
}

void igt_colorop_set_ctm_3x4(igt_display_t *display,
			     igt_colorop_t *colorop,
			     const igt_matrix_3x4_t *matrix)
{
	struct drm_color_ctm_3x4 ctm;
	int i;

	for (i = 0; i < ARRAY_SIZE(ctm.matrix); i++) {
		if (matrix->m[i] < 0) {
			ctm.matrix[i] =
				(int64_t) (-matrix->m[i] *
				((int64_t) 1L << 32));
			ctm.matrix[i] |= 1ULL << 63;
		} else {
			ctm.matrix[i] =
				(int64_t) (matrix->m[i] *
				((int64_t) 1L << 32));
		}
	}

	/* set blob property */
	igt_colorop_replace_prop_blob(colorop, IGT_COLOROP_DATA, &ctm, sizeof(ctm));
}
