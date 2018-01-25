/*
 * Copyright © 2013,2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 * 	Daniel Vetter <daniel.vetter@ffwll.ch>
 * 	Damien Lespiau <damien.lespiau@intel.com>
 */

#include <stdio.h>
#include <math.h>
#include <inttypes.h>

#include "drmtest.h"
#include "igt_fb.h"
#include "igt_kms.h"
#include "ioctl_wrappers.h"
#include "intel_chipset.h"

/**
 * SECTION:igt_fb
 * @short_description: Framebuffer handling and drawing library
 * @title: Framebuffer
 * @include: igt.h
 *
 * This library contains helper functions for handling kms framebuffer objects
 * using #igt_fb structures to track all the metadata.  igt_create_fb() creates
 * a basic framebuffer and igt_remove_fb() cleans everything up again.
 *
 * It also supports drawing using the cairo library and provides some simplified
 * helper functions to easily draw test patterns. The main function to create a
 * cairo drawing context for a framebuffer object is igt_get_cairo_ctx().
 *
 * Finally it also pulls in the drm fourcc headers and provides some helper
 * functions to work with these pixel format codes.
 */

/* drm fourcc/cairo format maps */
#define DF(did, cid, ...)	\
	{ DRM_FORMAT_##did, CAIRO_FORMAT_##cid, # did, __VA_ARGS__ }
static struct format_desc_struct {
	uint32_t drm_id;
	cairo_format_t cairo_id;
	const char *name;
	int bpp;
	int depth;
	int planes;
	int plane_bpp[4];
} format_desc[] = {
	DF(RGB565,	RGB16_565,	16, 16),
	//DF(RGB888,	INVALID,	24, 24),
	DF(XRGB8888,	RGB24,		32, 24),
	DF(XRGB2101010,	RGB30,		32, 30),
	DF(ARGB8888,	ARGB32,		32, 32),
	DF(NV12,	RGB24,		32, -1, 2, {8, 16}),
};
#undef DF

#define for_each_format(f)	\
	for (f = format_desc; f - format_desc < ARRAY_SIZE(format_desc); f++)

static struct format_desc_struct *lookup_drm_format(uint32_t drm_format)
{
	struct format_desc_struct *format;

	for_each_format(format) {
		if (format->drm_id != drm_format)
			continue;

		return format;
	}

	return NULL;
}

/**
 * igt_get_fb_tile_size:
 * @fd: the DRM file descriptor
 * @tiling: tiling layout of the framebuffer (as framebuffer modifier)
 * @fb_bpp: bits per pixel of the framebuffer
 * @width_ret: width of the tile in bytes
 * @height_ret: height of the tile in lines
 *
 * This function returns width and height of a tile based on the given tiling
 * format.
 */
void igt_get_fb_tile_size(int fd, uint64_t tiling, int fb_bpp,
			  unsigned *width_ret, unsigned *height_ret)
{
	switch (tiling) {
	case LOCAL_DRM_FORMAT_MOD_NONE:
		*width_ret = 64;
		*height_ret = 1;
		break;
	case LOCAL_I915_FORMAT_MOD_X_TILED:
		igt_require_intel(fd);
		if (intel_gen(intel_get_drm_devid(fd)) == 2) {
			*width_ret = 128;
			*height_ret = 16;
		} else {
			*width_ret = 512;
			*height_ret = 8;
		}
		break;
	case LOCAL_I915_FORMAT_MOD_Y_TILED:
		igt_require_intel(fd);
		if (intel_gen(intel_get_drm_devid(fd)) == 2) {
			*width_ret = 128;
			*height_ret = 16;
		} else if (IS_915(intel_get_drm_devid(fd))) {
			*width_ret = 512;
			*height_ret = 8;
		} else {
			*width_ret = 128;
			*height_ret = 32;
		}
		break;
	case LOCAL_I915_FORMAT_MOD_Yf_TILED:
		igt_require_intel(fd);
		switch (fb_bpp) {
		case 8:
			*width_ret = 64;
			*height_ret = 64;
			break;
		case 16:
		case 32:
			*width_ret = 128;
			*height_ret = 32;
			break;
		case 64:
		case 128:
			*width_ret = 256;
			*height_ret = 16;
			break;
		default:
			igt_assert(false);
		}
		break;
	default:
		igt_assert(false);
	}
}

static unsigned planar_width(struct format_desc_struct *format, unsigned width, int plane)
{
	if (format->drm_id == DRM_FORMAT_NV12 && plane == 1)
		return (width + 1) / 2;

	return width;
}

static unsigned planar_stride(struct format_desc_struct *format, unsigned width, int plane)
{
	unsigned cpp = format->plane_bpp[plane] / 8;

	return planar_width(format, width, plane) * cpp;
}

static unsigned planar_height(struct format_desc_struct *format, unsigned height, int plane)
{
	if (format->drm_id == DRM_FORMAT_NV12 && plane == 1)
		return (height + 1) / 2;

	return height;
}

static void calc_fb_size_planar(int fd, int width, int height,
				struct format_desc_struct *format,
				uint64_t tiling, unsigned *size_ret,
				unsigned *stride_ret, unsigned *offsets)
{
	int plane;
	unsigned stride = 0, tile_width, tile_height;

	*size_ret = 0;

	for (plane = 0; plane < format->planes; plane++) {
		unsigned plane_stride;

		igt_get_fb_tile_size(fd, tiling, format->plane_bpp[plane], &tile_width, &tile_height);

		plane_stride = ALIGN(planar_stride(format, width, plane), tile_width);
		if (stride < plane_stride)
			stride = plane_stride;
	}

	for (plane = 0; plane < format->planes; plane++) {
		if (offsets)
			offsets[plane] = *size_ret;

		igt_get_fb_tile_size(fd, tiling, format->plane_bpp[plane], &tile_width, &tile_height);

		*size_ret += stride * ALIGN(planar_height(format, height, plane), tile_height);
	}

	if (offsets)
		for (; plane < 4; plane++)
			offsets[plane] = 0;

	*stride_ret = stride;
}

static void calc_fb_size_packed(int fd, int width, int height,
				struct format_desc_struct *format, uint64_t tiling,
				unsigned *size_ret, unsigned *stride_ret)
{
	unsigned int tile_width, tile_height, stride, size;
	int byte_width = width * (format->bpp / 8);

	igt_get_fb_tile_size(fd, tiling, format->bpp, &tile_width, &tile_height);

	if (tiling != LOCAL_DRM_FORMAT_MOD_NONE &&
	    intel_gen(intel_get_drm_devid(fd)) <= 3) {
		int v;

		/* Round the tiling up to the next power-of-two and the region
		 * up to the next pot fence size so that this works on all
		 * generations.
		 *
		 * This can still fail if the framebuffer is too large to be
		 * tiled. But then that failure is expected.
		 */

		v = byte_width;
		for (stride = 512; stride < v; stride *= 2)
			;

		v = stride * height;
		for (size = 1024*1024; size < v; size *= 2)
			;
	} else {
		stride = ALIGN(byte_width, tile_width);
		size = stride * ALIGN(height, tile_height);
	}

	*stride_ret = stride;
	*size_ret = size;
}

/**
 * igt_calc_fb_size:
 * @fd: the DRM file descriptor
 * @width: width of the framebuffer in pixels
 * @height: height of the framebuffer in pixels
 * @format: drm fourcc pixel format code
 * @tiling: tiling layout of the framebuffer (as framebuffer modifier)
 * @size_ret: returned size for the framebuffer
 * @stride_ret: returned stride for the framebuffer
 *
 * This function returns valid stride and size values for a framebuffer with the
 * specified parameters.
 */
void igt_calc_fb_size(int fd, int width, int height, uint32_t drm_format, uint64_t tiling,
		      unsigned *size_ret, unsigned *stride_ret)
{
	struct format_desc_struct *format = lookup_drm_format(drm_format);
	igt_assert(format);

	if (format->planes > 1)
		calc_fb_size_planar(fd, width, height, format, tiling, size_ret, stride_ret, NULL);
	else
		calc_fb_size_packed(fd, width, height, format, tiling, size_ret, stride_ret);
}

/**
 * igt_fb_mod_to_tiling:
 * @modifier: DRM framebuffer modifier
 *
 * This function converts a DRM framebuffer modifier to its corresponding
 * tiling constant.
 *
 * Returns:
 * A tiling constant
 */
uint64_t igt_fb_mod_to_tiling(uint64_t modifier)
{
	switch (modifier) {
	case LOCAL_DRM_FORMAT_MOD_NONE:
		return I915_TILING_NONE;
	case LOCAL_I915_FORMAT_MOD_X_TILED:
		return I915_TILING_X;
	case LOCAL_I915_FORMAT_MOD_Y_TILED:
		return I915_TILING_Y;
	case LOCAL_I915_FORMAT_MOD_Yf_TILED:
		return I915_TILING_Yf;
	default:
		igt_assert(0);
	}
}

/**
 * igt_fb_tiling_to_mod:
 * @tiling: DRM framebuffer tiling
 *
 * This function converts a DRM framebuffer tiling to its corresponding
 * modifier constant.
 *
 * Returns:
 * A modifier constant
 */
uint64_t igt_fb_tiling_to_mod(uint64_t tiling)
{
	switch (tiling) {
	case I915_TILING_NONE:
		return LOCAL_DRM_FORMAT_MOD_NONE;
	case I915_TILING_X:
		return LOCAL_I915_FORMAT_MOD_X_TILED;
	case I915_TILING_Y:
		return LOCAL_I915_FORMAT_MOD_Y_TILED;
	case I915_TILING_Yf:
		return LOCAL_I915_FORMAT_MOD_Yf_TILED;
	default:
		igt_assert(0);
	}
}

/* helpers to create nice-looking framebuffers */
static int create_bo_for_fb(int fd, int width, int height,
			    struct format_desc_struct *format,
			    uint64_t tiling, unsigned size, unsigned stride,
			    unsigned *size_ret, unsigned *stride_ret,
			    uint32_t *offsets, bool *is_dumb)
{
	int bo;

	igt_assert(format);

	if (tiling || size || stride || format->planes > 1) {
		unsigned calculated_size, calculated_stride;

		if (format->planes > 1)
			calc_fb_size_planar(fd, width, height, format, tiling,
					    &calculated_size, &calculated_stride, offsets);
		else {
			memset(offsets, 0, 4 * sizeof(*offsets));
			calc_fb_size_packed(fd, width, height, format, tiling,
					    &calculated_size, &calculated_stride);
		}

		if (stride == 0)
			stride = calculated_stride;
		if (size == 0)
			size = calculated_size;

		if (is_dumb)
			*is_dumb = false;

		if (is_i915_device(fd)) {
			uint8_t *ptr;

			bo = gem_create(fd, size);
			gem_set_tiling(fd, bo, igt_fb_mod_to_tiling(tiling), stride);

			/* Ensure the framebuffer is preallocated */
			ptr = gem_mmap__gtt(fd, bo, size, PROT_READ | PROT_WRITE);
			igt_assert(*(uint32_t *)ptr == 0);

			if (format->drm_id == DRM_FORMAT_NV12) {
				/* component formats have a different zero point */
				memset(ptr, 16, offsets[1]);
				memset(ptr + offsets[1], 0x80, (height + 1)/2 * stride);
			}
			gem_munmap(ptr, size);

			if (size_ret)
				*size_ret = size;

			if (stride_ret)
				*stride_ret = stride;

			return bo;
		} else {
			bool driver_has_gem_api = false;

			igt_require(driver_has_gem_api);
			return -EINVAL;
		}
	} else {
		if (is_dumb)
			*is_dumb = true;

		return kmstest_dumb_create(fd, width, height, format->bpp, stride_ret,
					   size_ret);
	}
}

/**
 * igt_create_bo_with_dimensions:
 * @fd: open drm file descriptor
 * @width: width of the buffer object in pixels
 * @height: height of the buffer object in pixels
 * @format: drm fourcc pixel format code
 * @modifier: modifier corresponding to the tiling layout of the buffer object
 * @stride: stride of the buffer object in bytes (0 for automatic stride)
 * @size_ret: size of the buffer object as created by the kernel
 * @stride_ret: stride of the buffer object as created by the kernel
 * @is_dumb: whether the created buffer object is a dumb buffer or not
 *
 * This function allocates a gem buffer object matching the requested
 * properties.
 *
 * Returns:
 * The kms id of the created buffer object.
 */
int igt_create_bo_with_dimensions(int fd, int width, int height,
				  uint32_t format, uint64_t modifier,
				  unsigned stride, unsigned *size_ret,
				  unsigned *stride_ret, bool *is_dumb)
{
	return create_bo_for_fb(fd, width, height, lookup_drm_format(format),
				modifier, 0, stride, size_ret, stride_ret, NULL, is_dumb);
}

/**
 * igt_paint_color:
 * @cr: cairo drawing context
 * @x: pixel x-coordination of the fill rectangle
 * @y: pixel y-coordination of the fill rectangle
 * @w: width of the fill rectangle
 * @h: height of the fill rectangle
 * @r: red value to use as fill color
 * @g: green value to use as fill color
 * @b: blue value to use as fill color
 *
 * This functions draws a solid rectangle with the given color using the drawing
 * context @cr.
 */
void igt_paint_color(cairo_t *cr, int x, int y, int w, int h,
		     double r, double g, double b)
{
	cairo_rectangle(cr, x, y, w, h);
	cairo_set_source_rgb(cr, r, g, b);
	cairo_fill(cr);
}

/**
 * igt_paint_color_alpha:
 * @cr: cairo drawing context
 * @x: pixel x-coordination of the fill rectangle
 * @y: pixel y-coordination of the fill rectangle
 * @w: width of the fill rectangle
 * @h: height of the fill rectangle
 * @r: red value to use as fill color
 * @g: green value to use as fill color
 * @b: blue value to use as fill color
 * @a: alpha value to use as fill color
 *
 * This functions draws a rectangle with the given color and alpha values using
 * the drawing context @cr.
 */
void igt_paint_color_alpha(cairo_t *cr, int x, int y, int w, int h,
			   double r, double g, double b, double a)
{
	cairo_rectangle(cr, x, y, w, h);
	cairo_set_source_rgba(cr, r, g, b, a);
	cairo_fill(cr);
}

/**
 * igt_paint_color_gradient:
 * @cr: cairo drawing context
 * @x: pixel x-coordination of the fill rectangle
 * @y: pixel y-coordination of the fill rectangle
 * @w: width of the fill rectangle
 * @h: height of the fill rectangle
 * @r: red value to use as fill color
 * @g: green value to use as fill color
 * @b: blue value to use as fill color
 *
 * This functions draws a gradient into the rectangle which fades in from black
 * to the given values using the drawing context @cr.
 */
void
igt_paint_color_gradient(cairo_t *cr, int x, int y, int w, int h,
			 int r, int g, int b)
{
	cairo_pattern_t *pat;

	pat = cairo_pattern_create_linear(x, y, x + w, y + h);
	cairo_pattern_add_color_stop_rgba(pat, 1, 0, 0, 0, 1);
	cairo_pattern_add_color_stop_rgba(pat, 0, r, g, b, 1);

	cairo_rectangle(cr, x, y, w, h);
	cairo_set_source(cr, pat);
	cairo_fill(cr);
	cairo_pattern_destroy(pat);
}

/**
 * igt_paint_color_gradient_range:
 * @cr: cairo drawing context
 * @x: pixel x-coordination of the fill rectangle
 * @y: pixel y-coordination of the fill rectangle
 * @w: width of the fill rectangle
 * @h: height of the fill rectangle
 * @sr: red value to use as start gradient color
 * @sg: green value to use as start gradient color
 * @sb: blue value to use as start gradient color
 * @er: red value to use as end gradient color
 * @eg: green value to use as end gradient color
 * @eb: blue value to use as end gradient color
 *
 * This functions draws a gradient into the rectangle which fades in
 * from one color to the other using the drawing context @cr.
 */
void
igt_paint_color_gradient_range(cairo_t *cr, int x, int y, int w, int h,
			       double sr, double sg, double sb,
			       double er, double eg, double eb)
{
	cairo_pattern_t *pat;

	pat = cairo_pattern_create_linear(x, y, x + w, y + h);
	cairo_pattern_add_color_stop_rgba(pat, 1, sr, sg, sb, 1);
	cairo_pattern_add_color_stop_rgba(pat, 0, er, eg, eb, 1);

	cairo_rectangle(cr, x, y, w, h);
	cairo_set_source(cr, pat);
	cairo_fill(cr);
	cairo_pattern_destroy(pat);
}

static void
paint_test_patterns(cairo_t *cr, int width, int height)
{
	double gr_height, gr_width;
	int x, y;

	y = height * 0.10;
	gr_width = width * 0.75;
	gr_height = height * 0.08;
	x = (width / 2) - (gr_width / 2);

	igt_paint_color_gradient(cr, x, y, gr_width, gr_height, 1, 0, 0);

	y += gr_height;
	igt_paint_color_gradient(cr, x, y, gr_width, gr_height, 0, 1, 0);

	y += gr_height;
	igt_paint_color_gradient(cr, x, y, gr_width, gr_height, 0, 0, 1);

	y += gr_height;
	igt_paint_color_gradient(cr, x, y, gr_width, gr_height, 1, 1, 1);
}

/**
 * igt_cairo_printf_line:
 * @cr: cairo drawing context
 * @align: text alignment
 * @yspacing: additional y-direction feed after this line
 * @fmt: format string
 * @...: optional arguments used in the format string
 *
 * This is a little helper to draw text onto framebuffers. All the initial setup
 * (like setting the font size and the moving to the starting position) still
 * needs to be done manually with explicit cairo calls on @cr.
 *
 * Returns:
 * The width of the drawn text.
 */
int igt_cairo_printf_line(cairo_t *cr, enum igt_text_align align,
				double yspacing, const char *fmt, ...)
{
	double x, y, xofs, yofs;
	cairo_text_extents_t extents;
	char *text;
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = vasprintf(&text, fmt, ap);
	igt_assert(ret >= 0);
	va_end(ap);

	cairo_text_extents(cr, text, &extents);

	xofs = yofs = 0;
	if (align & align_right)
		xofs = -extents.width;
	else if (align & align_hcenter)
		xofs = -extents.width / 2;

	if (align & align_top)
		yofs = extents.height;
	else if (align & align_vcenter)
		yofs = extents.height / 2;

	cairo_get_current_point(cr, &x, &y);
	if (xofs || yofs)
		cairo_rel_move_to(cr, xofs, yofs);

	cairo_text_path(cr, text);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_stroke_preserve(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_fill(cr);

	cairo_move_to(cr, x, y + extents.height + yspacing);

	free(text);

	return extents.width;
}

static void
paint_marker(cairo_t *cr, int x, int y)
{
	enum igt_text_align align;
	int xoff, yoff;

	cairo_move_to(cr, x, y - 20);
	cairo_line_to(cr, x, y + 20);
	cairo_move_to(cr, x - 20, y);
	cairo_line_to(cr, x + 20, y);
	cairo_new_sub_path(cr);
	cairo_arc(cr, x, y, 10, 0, M_PI * 2);
	cairo_set_line_width(cr, 4);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_stroke_preserve(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_set_line_width(cr, 2);
	cairo_stroke(cr);

	xoff = x ? -20 : 20;
	align = x ? align_right : align_left;

	yoff = y ? -20 : 20;
	align |= y ? align_bottom : align_top;

	cairo_move_to(cr, x + xoff, y + yoff);
	cairo_set_font_size(cr, 18);
	igt_cairo_printf_line(cr, align, 0, "(%d, %d)", x, y);
}

/**
 * igt_paint_test_pattern:
 * @cr: cairo drawing context
 * @width: width of the visible area
 * @height: height of the visible area
 *
 * This functions draws an entire set of test patterns for the given visible
 * area using the drawing context @cr. This is useful for manual visual
 * inspection of displayed framebuffers.
 *
 * The test patterns include
 *  - corner markers to check for over/underscan and
 *  - a set of color and b/w gradients.
 */
void igt_paint_test_pattern(cairo_t *cr, int width, int height)
{
	paint_test_patterns(cr, width, height);

	cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);

	/* Paint corner markers */
	paint_marker(cr, 0, 0);
	paint_marker(cr, width, 0);
	paint_marker(cr, 0, height);
	paint_marker(cr, width, height);

	igt_assert(!cairo_status(cr));
}

static cairo_status_t
stdio_read_func(void *closure, unsigned char* data, unsigned int size)
{
	if (fread(data, 1, size, (FILE*)closure) != size)
		return CAIRO_STATUS_READ_ERROR;

	return CAIRO_STATUS_SUCCESS;
}

cairo_surface_t *igt_cairo_image_surface_create_from_png(const char *filename)
{
	cairo_surface_t *image;
	FILE *f;

	f = igt_fopen_data(filename);
	image = cairo_image_surface_create_from_png_stream(&stdio_read_func, f);
	fclose(f);

	return image;
}

/**
 * igt_paint_image:
 * @cr: cairo drawing context
 * @filename: filename of the png image to draw
 * @dst_x: pixel x-coordination of the destination rectangle
 * @dst_y: pixel y-coordination of the destination rectangle
 * @dst_width: width of the destination rectangle
 * @dst_height: height of the destination rectangle
 *
 * This function can be used to draw a scaled version of the supplied png image,
 * which is loaded from the package data directory.
 */
void igt_paint_image(cairo_t *cr, const char *filename,
		     int dst_x, int dst_y, int dst_width, int dst_height)
{
	cairo_surface_t *image;
	int img_width, img_height;
	double scale_x, scale_y;

	image = igt_cairo_image_surface_create_from_png(filename);
	igt_assert(cairo_surface_status(image) == CAIRO_STATUS_SUCCESS);

	img_width = cairo_image_surface_get_width(image);
	img_height = cairo_image_surface_get_height(image);

	scale_x = (double)dst_width / img_width;
	scale_y = (double)dst_height / img_height;

	cairo_save(cr);

	cairo_translate(cr, dst_x, dst_y);
	cairo_scale(cr, scale_x, scale_y);
	cairo_set_source_surface(cr, image, 0, 0);
	cairo_paint(cr);

	cairo_surface_destroy(image);

	cairo_restore(cr);
}

/**
 * igt_create_fb_with_bo_size:
 * @fd: open i915 drm file descriptor
 * @width: width of the framebuffer in pixel
 * @height: height of the framebuffer in pixel
 * @format: drm fourcc pixel format code
 * @tiling: tiling layout of the framebuffer (as framebuffer modifier)
 * @fb: pointer to an #igt_fb structure
 * @bo_size: size of the backing bo (0 for automatic size)
 * @bo_stride: stride of the backing bo (0 for automatic stride)
 *
 * This function allocates a gem buffer object suitable to back a framebuffer
 * with the requested properties and then wraps it up in a drm framebuffer
 * object of the requested size. All metadata is stored in @fb.
 *
 * The backing storage of the framebuffer is filled with all zeros, i.e. black
 * for rgb pixel formats.
 *
 * Returns:
 * The kms id of the created framebuffer.
 */
unsigned int
igt_create_fb_with_bo_size(int fd, int width, int height,
			   uint32_t format, uint64_t tiling,
			   struct igt_fb *fb, unsigned bo_size,
			   unsigned bo_stride)
{
	struct format_desc_struct *f = lookup_drm_format(format);
	uint32_t fb_id;
	int i;

	igt_assert_f(f, "DRM format %08x not found\n", format);

	memset(fb, 0, sizeof(*fb));

	igt_debug("%s(width=%d, height=%d, format=0x%x, tiling=0x%"PRIx64", size=%d)\n",
		  __func__, width, height, format, tiling, bo_size);
	fb->gem_handle = create_bo_for_fb(fd, width, height, f,
					  tiling, bo_size, bo_stride,
					  &fb->size, &fb->stride,
					  fb->offsets, &fb->is_dumb);
	igt_assert(fb->gem_handle > 0);

	igt_debug("%s(handle=%d, pitch=%d)\n",
		  __func__, fb->gem_handle, fb->stride);

	if (tiling != LOCAL_DRM_FORMAT_MOD_NONE &&
	    tiling != LOCAL_I915_FORMAT_MOD_X_TILED) {
		do_or_die(__kms_addfb(fd, fb->gem_handle, width, height,
				      fb->stride, format, tiling, fb->offsets,
				      LOCAL_DRM_MODE_FB_MODIFIERS, &fb_id));
	} else {
		uint32_t handles[4];
		uint32_t pitches[4];

		memset(handles, 0, sizeof(handles));
		memset(pitches, 0, sizeof(pitches));

		handles[0] = fb->gem_handle;
		pitches[0] = fb->stride;
		for (i = 0; i < f->planes; i++) {
			handles[i] = fb->gem_handle;
			pitches[i] = fb->stride;
		}

		do_or_die(drmModeAddFB2(fd, width, height, format,
					handles, pitches, fb->offsets,
					&fb_id, 0));
	}

	fb->width = width;
	fb->height = height;
	fb->tiling = tiling;
	fb->drm_format = format;
	fb->fb_id = fb_id;
	fb->fd = fd;
	fb->num_planes = f->planes ?: 1;
	fb->plane_bpp[0] = f->bpp;
	fb->plane_height[0] = height;
	fb->plane_width[0] = width;

	/* if f->planes is set, then plane_bpp is valid too so use that. */
	for (i = 0; i < f->planes; i++) {
		fb->plane_bpp[i] = f->plane_bpp[i];
		fb->plane_height[i] = planar_height(f, height, i);
		fb->plane_width[i] = planar_width(f, width, i);
	}

	return fb_id;
}

/**
 * igt_create_fb:
 * @fd: open i915 drm file descriptor
 * @width: width of the framebuffer in pixel
 * @height: height of the framebuffer in pixel
 * @format: drm fourcc pixel format code
 * @tiling: tiling layout of the framebuffer
 * @fb: pointer to an #igt_fb structure
 *
 * This function allocates a gem buffer object suitable to back a framebuffer
 * with the requested properties and then wraps it up in a drm framebuffer
 * object. All metadata is stored in @fb.
 *
 * The backing storage of the framebuffer is filled with all zeros, i.e. black
 * for rgb pixel formats.
 *
 * Returns:
 * The kms id of the created framebuffer.
 */
unsigned int igt_create_fb(int fd, int width, int height, uint32_t format,
			   uint64_t tiling, struct igt_fb *fb)
{
	return igt_create_fb_with_bo_size(fd, width, height, format, tiling, fb,
					  0, 0);
}

/**
 * igt_create_color_fb:
 * @fd: open i915 drm file descriptor
 * @width: width of the framebuffer in pixel
 * @height: height of the framebuffer in pixel
 * @format: drm fourcc pixel format code
 * @tiling: tiling layout of the framebuffer
 * @r: red value to use as fill color
 * @g: green value to use as fill color
 * @b: blue value to use as fill color
 * @fb: pointer to an #igt_fb structure
 *
 * This function allocates a gem buffer object suitable to back a framebuffer
 * with the requested properties and then wraps it up in a drm framebuffer
 * object. All metadata is stored in @fb.
 *
 * Compared to igt_create_fb() this function also fills the entire framebuffer
 * with the given color, which is useful for some simple pipe crc based tests.
 *
 * Returns:
 * The kms id of the created framebuffer on success or a negative error code on
 * failure.
 */
unsigned int igt_create_color_fb(int fd, int width, int height,
				 uint32_t format, uint64_t tiling,
				 double r, double g, double b,
				 struct igt_fb *fb /* out */)
{
	unsigned int fb_id;
	cairo_t *cr;

	fb_id = igt_create_fb(fd, width, height, format, tiling, fb);
	igt_assert(fb_id);

	cr = igt_get_cairo_ctx(fd, fb);
	igt_paint_color(cr, 0, 0, width, height, r, g, b);
	igt_put_cairo_ctx(fd, fb, cr);

	return fb_id;
}

/**
 * igt_create_pattern_fb:
 * @fd: open i915 drm file descriptor
 * @width: width of the framebuffer in pixel
 * @height: height of the framebuffer in pixel
 * @format: drm fourcc pixel format code
 * @tiling: tiling layout of the framebuffer
 * @fb: pointer to an #igt_fb structure
 *
 * This function allocates a gem buffer object suitable to back a framebuffer
 * with the requested properties and then wraps it up in a drm framebuffer
 * object. All metadata is stored in @fb.
 *
 * Compared to igt_create_fb() this function also draws the standard test pattern
 * into the framebuffer.
 *
 * Returns:
 * The kms id of the created framebuffer on success or a negative error code on
 * failure.
 */
unsigned int igt_create_pattern_fb(int fd, int width, int height,
				   uint32_t format, uint64_t tiling,
				   struct igt_fb *fb /* out */)
{
	unsigned int fb_id;
	cairo_t *cr;

	fb_id = igt_create_fb(fd, width, height, format, tiling, fb);
	igt_assert(fb_id);

	cr = igt_get_cairo_ctx(fd, fb);
	igt_paint_test_pattern(cr, width, height);
	igt_put_cairo_ctx(fd, fb, cr);

	return fb_id;
}

/**
 * igt_create_color_pattern_fb:
 * @fd: open i915 drm file descriptor
 * @width: width of the framebuffer in pixel
 * @height: height of the framebuffer in pixel
 * @format: drm fourcc pixel format code
 * @tiling: tiling layout of the framebuffer
 * @r: red value to use as fill color
 * @g: green value to use as fill color
 * @b: blue value to use as fill color
 * @fb: pointer to an #igt_fb structure
 *
 * This function allocates a gem buffer object suitable to back a framebuffer
 * with the requested properties and then wraps it up in a drm framebuffer
 * object. All metadata is stored in @fb.
 *
 * Compared to igt_create_fb() this function also fills the entire framebuffer
 * with the given color, and then draws the standard test pattern into the
 * framebuffer.
 *
 * Returns:
 * The kms id of the created framebuffer on success or a negative error code on
 * failure.
 */
unsigned int igt_create_color_pattern_fb(int fd, int width, int height,
					 uint32_t format, uint64_t tiling,
					 double r, double g, double b,
					 struct igt_fb *fb /* out */)
{
	unsigned int fb_id;
	cairo_t *cr;

	fb_id = igt_create_fb(fd, width, height, format, tiling, fb);
	igt_assert(fb_id);

	cr = igt_get_cairo_ctx(fd, fb);
	igt_paint_color(cr, 0, 0, width, height, r, g, b);
	igt_paint_test_pattern(cr, width, height);
	igt_put_cairo_ctx(fd, fb, cr);

	return fb_id;
}

/**
 * igt_create_image_fb:
 * @drm_fd: open i915 drm file descriptor
 * @width: width of the framebuffer in pixel or 0
 * @height: height of the framebuffer in pixel or 0
 * @format: drm fourcc pixel format code
 * @tiling: tiling layout of the framebuffer
 * @filename: filename of the png image to draw
 * @fb: pointer to an #igt_fb structure
 *
 * Create a framebuffer with the specified image. If @width is zero the
 * image width will be used. If @height is zero the image height will be used.
 *
 * Returns:
 * The kms id of the created framebuffer on success or a negative error code on
 * failure.
 */
unsigned int igt_create_image_fb(int fd, int width, int height,
				 uint32_t format, uint64_t tiling,
				 const char *filename,
				 struct igt_fb *fb /* out */)
{
	cairo_surface_t *image;
	uint32_t fb_id;
	cairo_t *cr;

	image = igt_cairo_image_surface_create_from_png(filename);
	igt_assert(cairo_surface_status(image) == CAIRO_STATUS_SUCCESS);
	if (width == 0)
		width = cairo_image_surface_get_width(image);
	if (height == 0)
		height = cairo_image_surface_get_height(image);
	cairo_surface_destroy(image);

	fb_id = igt_create_fb(fd, width, height, format, tiling, fb);

	cr = igt_get_cairo_ctx(fd, fb);
	igt_paint_image(cr, filename, 0, 0, width, height);
	igt_put_cairo_ctx(fd, fb, cr);

	return fb_id;
}

struct box {
	int x, y, width, height;
};

struct stereo_fb_layout {
	int fb_width, fb_height;
	struct box left, right;
};

static void box_init(struct box *box, int x, int y, int bwidth, int bheight)
{
	box->x = x;
	box->y = y;
	box->width = bwidth;
	box->height = bheight;
}


static void stereo_fb_layout_from_mode(struct stereo_fb_layout *layout,
				       drmModeModeInfo *mode)
{
	unsigned int format = mode->flags & DRM_MODE_FLAG_3D_MASK;
	const int hdisplay = mode->hdisplay, vdisplay = mode->vdisplay;
	int middle;

	switch (format) {
	case DRM_MODE_FLAG_3D_TOP_AND_BOTTOM:
		layout->fb_width = hdisplay;
		layout->fb_height = vdisplay;

		middle = vdisplay / 2;
		box_init(&layout->left, 0, 0, hdisplay, middle);
		box_init(&layout->right,
			 0, middle, hdisplay, vdisplay - middle);
		break;
	case DRM_MODE_FLAG_3D_SIDE_BY_SIDE_HALF:
		layout->fb_width = hdisplay;
		layout->fb_height = vdisplay;

		middle = hdisplay / 2;
		box_init(&layout->left, 0, 0, middle, vdisplay);
		box_init(&layout->right,
			 middle, 0, hdisplay - middle, vdisplay);
		break;
	case DRM_MODE_FLAG_3D_FRAME_PACKING:
	{
		int vactive_space = mode->vtotal - vdisplay;

		layout->fb_width = hdisplay;
		layout->fb_height = 2 * vdisplay + vactive_space;

		box_init(&layout->left,
			 0, 0, hdisplay, vdisplay);
		box_init(&layout->right,
			 0, vdisplay + vactive_space, hdisplay, vdisplay);
		break;
	}
	default:
		igt_assert(0);
	}
}

/**
 * igt_create_stereo_fb:
 * @drm_fd: open i915 drm file descriptor
 * @mode: A stereo 3D mode.
 * @format: drm fourcc pixel format code
 * @tiling: tiling layout of the framebuffer
 *
 * Create a framebuffer for use with the stereo 3D mode specified by @mode.
 *
 * Returns:
 * The kms id of the created framebuffer on success or a negative error code on
 * failure.
 */
unsigned int igt_create_stereo_fb(int drm_fd, drmModeModeInfo *mode,
				  uint32_t format, uint64_t tiling)
{
	struct stereo_fb_layout layout;
	cairo_t *cr;
	uint32_t fb_id;
	struct igt_fb fb;

	stereo_fb_layout_from_mode(&layout, mode);
	fb_id = igt_create_fb(drm_fd, layout.fb_width, layout.fb_height, format,
			      tiling, &fb);
	cr = igt_get_cairo_ctx(drm_fd, &fb);

	igt_paint_image(cr, "1080p-left.png",
			layout.left.x, layout.left.y,
			layout.left.width, layout.left.height);
	igt_paint_image(cr, "1080p-right.png",
			layout.right.x, layout.right.y,
			layout.right.width, layout.right.height);

	igt_put_cairo_ctx(drm_fd, &fb, cr);

	return fb_id;
}

static cairo_format_t drm_format_to_cairo(uint32_t drm_format)
{
	struct format_desc_struct *f;

	for_each_format(f)
		if (f->drm_id == drm_format)
			return f->cairo_id;

	igt_assert_f(0, "can't find a cairo format for %08x (%s)\n",
		     drm_format, igt_format_str(drm_format));
}

struct fb_blit_linear {
	uint32_t handle;
	unsigned size, stride;
	uint8_t *map;
	bool is_dumb;
	uint32_t offsets[4];
};

struct fb_blit_upload {
	int fd;
	struct igt_fb *fb;
	struct fb_blit_linear linear;
};

static void free_linear_mapping(int fd, struct igt_fb *fb, struct fb_blit_linear *linear)
{
	unsigned int obj_tiling = igt_fb_mod_to_tiling(fb->tiling);
	int i;

	gem_munmap(linear->map, linear->size);
	gem_set_domain(fd, linear->handle,
		       I915_GEM_DOMAIN_GTT, 0);

	for (i = 0; i < fb->num_planes; i++)
		igt_blitter_fast_copy__raw(fd,
					   linear->handle,
					   linear->offsets[i],
					   linear->stride,
					   I915_TILING_NONE,
					   0, 0, /* src_x, src_y */
					   fb->plane_width[i], fb->plane_height[i],
					   fb->plane_bpp[i],
					   fb->gem_handle,
					   fb->offsets[i],
					   fb->stride,
					   obj_tiling,
					   0, 0 /* dst_x, dst_y */);

	gem_sync(fd, linear->handle);
	gem_close(fd, linear->handle);
}

static void destroy_cairo_surface__blit(void *arg)
{
	struct fb_blit_upload *blit = arg;

	blit->fb->cairo_surface = NULL;

	free_linear_mapping(blit->fd, blit->fb, &blit->linear);

	free(blit);
}

static void setup_linear_mapping(int fd, struct igt_fb *fb, struct fb_blit_linear *linear)
{
	unsigned int obj_tiling = igt_fb_mod_to_tiling(fb->tiling);
	int i;

	/*
	 * We create a linear BO that we'll map for the CPU to write to (using
	 * cairo). This linear bo will be then blitted to its final
	 * destination, tiling it at the same time.
	 */
	linear->handle = create_bo_for_fb(fd, fb->width, fb->height,
					       lookup_drm_format(fb->drm_format),
					       LOCAL_DRM_FORMAT_MOD_NONE, 0,
					       0, &linear->size,
					       &linear->stride,
					       linear->offsets, &linear->is_dumb);

	igt_assert(linear->handle > 0);

	/* Copy fb content to linear BO */
	gem_set_domain(fd, linear->handle,
			I915_GEM_DOMAIN_GTT, 0);

	for (i = 0; i < fb->num_planes; i++)
		igt_blitter_fast_copy__raw(fd,
					  fb->gem_handle,
					  fb->offsets[i],
					  fb->stride,
					  obj_tiling,
					  0, 0, /* src_x, src_y */
					  fb->plane_width[i], fb->plane_height[i],
					  fb->plane_bpp[i],
					  linear->handle, linear->offsets[i],
					  linear->stride,
					  I915_TILING_NONE,
					  0, 0 /* dst_x, dst_y */);

	gem_sync(fd, linear->handle);

	gem_set_domain(fd, linear->handle,
		       I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);

	/* Setup cairo context */
	linear->map = gem_mmap__cpu(fd, linear->handle,
				    0, linear->size, PROT_READ | PROT_WRITE);
}

static void create_cairo_surface__blit(int fd, struct igt_fb *fb)
{
	struct fb_blit_upload *blit;
	cairo_format_t cairo_format;

	blit = malloc(sizeof(*blit));
	igt_assert(blit);

	blit->fd = fd;
	blit->fb = fb;
	setup_linear_mapping(fd, fb, &blit->linear);

	cairo_format = drm_format_to_cairo(fb->drm_format);
	fb->cairo_surface =
		cairo_image_surface_create_for_data(blit->linear.map,
						    cairo_format,
						    fb->width, fb->height,
						    blit->linear.stride);
	fb->domain = I915_GEM_DOMAIN_GTT;

	cairo_surface_set_user_data(fb->cairo_surface,
				    (cairo_user_data_key_t *)create_cairo_surface__blit,
				    blit, destroy_cairo_surface__blit);
}

/**
 * igt_dirty_fb:
 * @fd: open drm file descriptor
 * @fb: pointer to an #igt_fb structure
 *
 * Flushes out the whole framebuffer.
 *
 * Returns: 0 upon success.
 */
int igt_dirty_fb(int fd, struct igt_fb *fb)
{
	return drmModeDirtyFB(fb->fd, fb->fb_id, NULL, 0);
}

static void destroy_cairo_surface__gtt(void *arg)
{
	struct igt_fb *fb = arg;

	gem_munmap(cairo_image_surface_get_data(fb->cairo_surface), fb->size);
	fb->cairo_surface = NULL;

	if (fb->is_dumb)
		igt_dirty_fb(fb->fd, fb);
}

static void create_cairo_surface__gtt(int fd, struct igt_fb *fb)
{
	void *ptr;

	if (fb->is_dumb)
		ptr = kmstest_dumb_map_buffer(fd, fb->gem_handle, fb->size,
					      PROT_READ | PROT_WRITE);
	else
		ptr = gem_mmap__gtt(fd, fb->gem_handle, fb->size,
				    PROT_READ | PROT_WRITE);

	fb->cairo_surface =
		cairo_image_surface_create_for_data(ptr,
						    drm_format_to_cairo(fb->drm_format),
						    fb->width, fb->height, fb->stride);
	fb->domain = I915_GEM_DOMAIN_GTT;

	cairo_surface_set_user_data(fb->cairo_surface,
				    (cairo_user_data_key_t *)create_cairo_surface__gtt,
				    fb, destroy_cairo_surface__gtt);
}

struct fb_convert_blit_upload {
	int fd;
	struct igt_fb *fb;

	struct {
		uint8_t *map;
		unsigned stride, size;
	} rgb24;

	struct fb_blit_linear linear;
};

static uint8_t clamprgb(float val) {
	if (val < 0)
		return 0;
	if (val > 255)
		return 255;

	return (uint8_t)val;
}

static void convert_nv12_to_rgb24(struct igt_fb *fb, struct fb_convert_blit_upload *blit)
{
	int i, j;
	const uint8_t *y, *uv;
	uint8_t *rgb24 = blit->rgb24.map;
	unsigned rgb24_stride = blit->rgb24.stride, planar_stride = blit->linear.stride;
	uint8_t *buf = malloc(blit->linear.size);

	/*
	 * Reading from the BO is awfully slow because of lack of read caching,
	 * it's faster to copy the whole BO to a temporary buffer and convert
	 * from there.
	 */
	memcpy(buf, blit->linear.map, blit->linear.size);
	y = &buf[blit->linear.offsets[0]];
	uv = &buf[blit->linear.offsets[1]];

	for (i = 0; i < fb->height / 2; i++) {
		for (j = 0; j < fb->width; j++) {
			float r_, g_, b_, y0, y1, cb, cr;
			/* Convert 1x2 pixel blocks */

			y0 = 1.164f * (y[j] - 16.f);
			y1 = 1.164f * (y[j + planar_stride] - 16.f);

			cb = uv[j & ~1] - 128.f;
			cr = uv[j | 1] - 128.f;

			r_ = 0.000f * cb +  1.793f * cr;
			g_ = -0.213f * cb + -0.533f * cr;
			b_ = 2.112f * cb +  0.000f * cr;

			rgb24[j * 4 + 2] = clamprgb(y0 + r_);
			rgb24[j * 4 + 2 + rgb24_stride] = clamprgb(y1 + r_);

			rgb24[j * 4 + 1] = clamprgb(y0 + g_);
			rgb24[j * 4 + 1 + rgb24_stride] = clamprgb(y1 + g_);

			rgb24[j * 4] = clamprgb(y0 + b_);
			rgb24[j * 4 + rgb24_stride] = clamprgb(y1 + b_);
		}

		rgb24 += 2 * rgb24_stride;
		y += 2 * planar_stride;
		uv += planar_stride;
	}

	if (fb->height & 1) {
		/* Convert last row */
		for (j = 0; j < fb->width; j++) {
			float r_, g_, b_, y0, cb, cr;
			/* Convert single pixel */

			cb = uv[j & ~1] - 128.f;
			cr = uv[j | 1] - 128.f;

			y0 = 1.164f * (y[j] - 16.f);
			r_ =  0.000f * cb +  1.793f * cr;
			g_ = -0.213f * cb + -0.533f * cr;
			b_ =  2.112f * cb +  0.000f * cr;

			rgb24[j * 4 + 2] = clamprgb(y0 + r_);
			rgb24[j * 4 + 1] = clamprgb(y0 + g_);
			rgb24[j * 4] = clamprgb(y0 + b_);
		}
	}

	free(buf);
}

static void convert_rgb24_to_nv12(struct igt_fb *fb, struct fb_convert_blit_upload *blit)
{
	int i, j;
	uint8_t *y = &blit->linear.map[blit->linear.offsets[0]];
	uint8_t *uv = &blit->linear.map[blit->linear.offsets[1]];
	const uint8_t *rgb24 = blit->rgb24.map;
	unsigned rgb24_stride = blit->rgb24.stride;
	unsigned planar_stride = blit->linear.stride;

	igt_assert_f(fb->drm_format == DRM_FORMAT_NV12,
		     "Conversion not implemented for !NV12 planar formats\n");

	for (i = 0; i < fb->plane_height[0]; i++) {
		/* Use limited color range BT.709 */

		for (j = 0; j < fb->plane_width[0]; j++) {
			float yf = 0.183f * rgb24[j * 4 + 2] +
				   0.614f * rgb24[j * 4 + 1] +
				   0.062f * rgb24[j * 4] + 16;

			y[j] = (uint8_t)yf;
		}

		rgb24 += rgb24_stride;
		y += planar_stride;
	}

	rgb24 = blit->rgb24.map;

	for (i = 0; i < fb->height / 2; i++) {
		for (j = 0; j < fb->plane_width[1]; j++) {
			/*
			 * Pixel center for Cb'Cr' is between the left top and
			 * bottom pixel in a 2x2 block, so take the average.
			 */
			float uf = -0.101f/2 * rgb24[j * 8 + 2] +
				   -0.101f/2 * rgb24[j * 8 + 2 + rgb24_stride] +
				   -0.339f/2 * rgb24[j * 8 + 1] +
				   -0.339f/2 * rgb24[j * 8 + 1 + rgb24_stride] +
				    0.439f/2 * rgb24[j * 8] +
				    0.439f/2 * rgb24[j * 8 + rgb24_stride] + 128;
			float vf =  0.439f/2 * rgb24[j * 8 + 2] +
				    0.439f/2 * rgb24[j * 8 + 2 + rgb24_stride] +
				   -0.339f/2 * rgb24[j * 8 + 1] +
				   -0.339f/2 * rgb24[j * 8 + 1 + rgb24_stride] +
				   -0.040f/2 * rgb24[j * 8] +
				   -0.040f/2 * rgb24[j * 8 + rgb24_stride] + 128;
			uv[j * 2] = (uint8_t)uf;
			uv[j * 2 + 1] = (uint8_t)vf;
		}

		rgb24 += 2 * rgb24_stride;
		uv += planar_stride;
	}

	/* Last row cannot be interpolated between 2 pixels, take the single value */
	if (i < fb->plane_height[1]) {
		for (j = 0; j < fb->plane_width[1]; j++) {
			float uf = -0.101f * rgb24[j * 8 + 2] +
				   -0.339f * rgb24[j * 8 + 1] +
				    0.439f * rgb24[j * 8] + 128;
			float vf =  0.439f * rgb24[j * 8 + 2] +
				   -0.339f * rgb24[j * 8 + 1] +
				   -0.040f * rgb24[j * 8] + 128;

			uv[j * 2] = (uint8_t)uf;
			uv[j * 2 + 1] = (uint8_t)vf;
		}
	}
}

static void destroy_cairo_surface__convert(void *arg)
{
	struct fb_convert_blit_upload *blit = arg;
	struct igt_fb *fb = blit->fb;

	/* Convert back to planar! */
	igt_assert_f(fb->drm_format == DRM_FORMAT_NV12,
		     "Conversion not implemented for !NV12 planar formats\n");

	convert_rgb24_to_nv12(fb, blit);

	munmap(blit->rgb24.map, blit->rgb24.size);

	if (blit->linear.handle)
		free_linear_mapping(blit->fd, blit->fb, &blit->linear);
	else
		gem_munmap(blit->linear.map, fb->size);

	free(blit);

	fb->cairo_surface = NULL;
}

static void create_cairo_surface__convert(int fd, struct igt_fb *fb)
{
	struct fb_convert_blit_upload *blit = malloc(sizeof(*blit));
	igt_assert(blit);

	blit->fd = fd;
	blit->fb = fb;
	blit->rgb24.stride = ALIGN(fb->width * 4, 16);
	blit->rgb24.size = ALIGN(blit->rgb24.stride * fb->height, sysconf(_SC_PAGESIZE));
	blit->rgb24.map = mmap(NULL, blit->rgb24.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	igt_assert(blit->rgb24.map != MAP_FAILED);

	if (fb->tiling == LOCAL_I915_FORMAT_MOD_Y_TILED ||
	    fb->tiling == LOCAL_I915_FORMAT_MOD_Yf_TILED) {
		setup_linear_mapping(fd, fb, &blit->linear);
	} else {
		blit->linear.handle = 0;
		blit->linear.map = gem_mmap__gtt(fd, fb->gem_handle, fb->size,
					      PROT_READ | PROT_WRITE);
		igt_assert(blit->linear.map);
		blit->linear.stride = fb->stride;
		blit->linear.size = fb->size;
		memcpy(blit->linear.offsets, fb->offsets, sizeof(fb->offsets));
	}

	/* Convert to linear! */
	igt_assert_f(fb->drm_format == DRM_FORMAT_NV12,
		     "Conversion not implemented for !NV12 planar formats\n");
	convert_nv12_to_rgb24(fb, blit);

	fb->cairo_surface =
		cairo_image_surface_create_for_data(blit->rgb24.map,
						    CAIRO_FORMAT_RGB24,
						    fb->width, fb->height,
						    blit->rgb24.stride);

	cairo_surface_set_user_data(fb->cairo_surface,
				    (cairo_user_data_key_t *)create_cairo_surface__convert,
				    blit, destroy_cairo_surface__convert);
}

/**
 * igt_get_cairo_surface:
 * @fd: open drm file descriptor
 * @fb: pointer to an #igt_fb structure
 *
 * This function stores the contents of the supplied framebuffer's plane
 * into a cairo surface and returns it.
 *
 * Returns:
 * A pointer to a cairo surface with the contents of the framebuffer.
 */
cairo_surface_t *igt_get_cairo_surface(int fd, struct igt_fb *fb)
{
	if (fb->cairo_surface == NULL) {
		if (fb->num_planes > 1)
			create_cairo_surface__convert(fd, fb);
		else if (fb->tiling == LOCAL_I915_FORMAT_MOD_Y_TILED ||
		    fb->tiling == LOCAL_I915_FORMAT_MOD_Yf_TILED)
			create_cairo_surface__blit(fd, fb);
		else
			create_cairo_surface__gtt(fd, fb);
	}

	if (!fb->is_dumb)
		gem_set_domain(fd, fb->gem_handle, I915_GEM_DOMAIN_CPU,
			       I915_GEM_DOMAIN_CPU);

	igt_assert(cairo_surface_status(fb->cairo_surface) == CAIRO_STATUS_SUCCESS);
	return fb->cairo_surface;
}

/**
 * igt_get_cairo_ctx:
 * @fd: open i915 drm file descriptor
 * @fb: pointer to an #igt_fb structure
 *
 * This initializes a cairo surface for @fb and then allocates a drawing context
 * for it. The return cairo drawing context should be released by calling
 * igt_put_cairo_ctx(). This also sets a default font for drawing text on
 * framebuffers.
 *
 * Returns:
 * The created cairo drawing context.
 */
cairo_t *igt_get_cairo_ctx(int fd, struct igt_fb *fb)
{
	cairo_surface_t *surface;
	cairo_t *cr;

	surface = igt_get_cairo_surface(fd, fb);
	cr = cairo_create(surface);
	cairo_surface_destroy(surface);
	igt_assert(cairo_status(cr) == CAIRO_STATUS_SUCCESS);

	cairo_select_font_face(cr, "Helvetica", CAIRO_FONT_SLANT_NORMAL,
			       CAIRO_FONT_WEIGHT_NORMAL);
	igt_assert(cairo_status(cr) == CAIRO_STATUS_SUCCESS);

	return cr;
}

/**
 * igt_put_cairo_ctx:
 * @fd: open i915 drm file descriptor
 * @fb: pointer to an #igt_fb structure
 * @cr: the cairo context returned by igt_get_cairo_ctx.
 *
 * This releases the cairo surface @cr returned by igt_get_cairo_ctx()
 * for @fb, and writes the changes out to the framebuffer if cairo doesn't
 * have native support for the format.
 */
void igt_put_cairo_ctx(int fd, struct igt_fb *fb, cairo_t *cr)
{
	cairo_status_t ret = cairo_status(cr);
	igt_assert_f(ret == CAIRO_STATUS_SUCCESS, "Cairo failed to draw with %s\n", cairo_status_to_string(ret));

	cairo_destroy(cr);
}

/**
 * igt_remove_fb:
 * @fd: open i915 drm file descriptor
 * @fb: pointer to an #igt_fb structure
 *
 * This function releases all resources allocated in igt_create_fb() for @fb.
 * Note that if this framebuffer is still in use on a primary plane the kernel
 * will disable the corresponding crtc.
 */
void igt_remove_fb(int fd, struct igt_fb *fb)
{
	cairo_surface_destroy(fb->cairo_surface);
	do_or_die(drmModeRmFB(fd, fb->fb_id));
	gem_close(fd, fb->gem_handle);
}

/**
 * igt_bpp_depth_to_drm_format:
 * @bpp: desired bits per pixel
 * @depth: desired depth
 *
 * Returns:
 * The rgb drm fourcc pixel format code corresponding to the given @bpp and
 * @depth values.  Fails hard if no match was found.
 */
uint32_t igt_bpp_depth_to_drm_format(int bpp, int depth)
{
	struct format_desc_struct *f;

	for_each_format(f)
		if (f->bpp == bpp && f->depth == depth)
			return f->drm_id;


	igt_assert_f(0, "can't find drm format with bpp=%d, depth=%d\n", bpp,
		     depth);
}

/**
 * igt_drm_format_to_bpp:
 * @drm_format: drm fourcc pixel format code
 *
 * Returns:
 * The bits per pixel for the given drm fourcc pixel format code. Fails hard if
 * no match was found.
 */
uint32_t igt_drm_format_to_bpp(uint32_t drm_format)
{
	struct format_desc_struct *f = lookup_drm_format(drm_format);

	igt_assert_f(f, "can't find a bpp format for %08x (%s)\n",
		     drm_format, igt_format_str(drm_format));

	return f->bpp;
}

/**
 * igt_format_str:
 * @drm_format: drm fourcc pixel format code
 *
 * Returns:
 * Human-readable fourcc pixel format code for @drm_format or "invalid" no match
 * was found.
 */
const char *igt_format_str(uint32_t drm_format)
{
	struct format_desc_struct *f = lookup_drm_format(drm_format);

	return f ? f->name : "invalid";
}

/**
 * igt_get_all_cairo_formats:
 * @formats: pointer to pointer to store the allocated formats array
 * @format_count: pointer to integer to store the size of the allocated array
 *
 * This functions returns an array of all the drm fourcc codes supported by
 * cairo and this library.
 */
void igt_get_all_cairo_formats(const uint32_t **formats, int *format_count)
{
	static uint32_t *drm_formats;
	static int n_formats;

	if (!drm_formats) {
		struct format_desc_struct *f;
		uint32_t *format;

		n_formats = 0;
		for_each_format(f)
			if (f->cairo_id != CAIRO_FORMAT_INVALID)
				n_formats++;

		drm_formats = calloc(n_formats, sizeof(*drm_formats));
		format = &drm_formats[0];
		for_each_format(f)
			if (f->cairo_id != CAIRO_FORMAT_INVALID)
				*format++ = f->drm_id;
	}

	*formats = drm_formats;
	*format_count = n_formats;
}
