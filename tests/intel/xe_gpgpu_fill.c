// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

/**
 * TEST: Basic tests for gpgpu functionality
 * Category: Core
 * Mega feature: Compute
 * Sub-category: GPGPU tests
 * Functionality: gpgpu_fill
 * Test category: functionality test
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "drm.h"
#include "i915/gem.h"
#include "igt.h"
#include "igt_collection.h"
#include "intel_bufops.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"

#define WIDTH 64
#define HEIGHT 64
#define STRIDE (WIDTH)
#define SIZE (HEIGHT*STRIDE)
#define COLOR_88	0x88
#define COLOR_4C	0x4c

static bool dump_surface;
static uint32_t surfwidth = WIDTH;
static uint32_t surfheight = HEIGHT;
static uint32_t start_x;
static uint32_t start_y;

typedef struct {
	int drm_fd;
	uint32_t devid;
	struct buf_ops *bops;
} data_t;

static struct intel_buf *
create_buf(data_t *data, int width, int height, uint8_t color, uint64_t region)
{
	struct intel_buf *buf;
	uint8_t *ptr;
	int i;

	buf = calloc(1, sizeof(*buf));
	igt_assert(buf);

	buf = intel_buf_create(data->bops, width/4, height, 32, 0,
			       I915_TILING_NONE, 0);

	ptr = xe_bo_map(data->drm_fd, buf->handle, buf->surface[0].size);

	for (i = 0; i < buf->surface[0].size; i++)
		ptr[i] = color;

	munmap(ptr, buf->surface[0].size);

	return buf;
}

static void buf_check(uint8_t *ptr, int width, int x, int y, uint8_t color)
{
	uint8_t val;

	val = ptr[y * width + x];
	igt_assert_f(val == color,
		     "Expected 0x%02x, found 0x%02x at (%d,%d)\n",
		     color, val, x, y);
}

/**
 * SUBTEST: basic
 * Description: run gpgpu fill
 *
 * SUBTEST: offset-16x16
 * Description: run gpgpu fill with <x,y> start position == <16,16>
 *
 */

static void gpgpu_fill(data_t *data, igt_fillfunc_t fill, uint32_t region,
		       uint32_t surf_width, uint32_t surf_height,
		       uint32_t x, uint32_t y,
		       uint32_t width, uint32_t height)
{
	struct intel_buf *buf;
	uint8_t *ptr;
	int i, j;

	buf = create_buf(data, surf_width, surf_height, COLOR_88, region);
	ptr = xe_bo_map(data->drm_fd, buf->handle, buf->surface[0].size);

	for (i = 0; i < surf_width; i++)
		for (j = 0; j < surf_height; j++)
			buf_check(ptr, surf_width, i, j, COLOR_88);

	fill(data->drm_fd, buf, x, y, width, height, COLOR_4C);

	if (dump_surface) {
		for (j = 0; j < surf_height; j++) {
			igt_info("[%04x] ", j);
			for (i = 0; i < surf_width; i++) {
				igt_info("%02x", ptr[j * surf_height + i]);
				if (i % 4 == 3)
					igt_info(" ");
			}
			igt_info("\n");
		}
	}

	for (i = 0; i < surf_width; i++)
		for (j = 0; j < surf_height; j++)
			if (i >= x && i < width + x &&
			    j >= y && j < height + y)
				buf_check(ptr, surf_width, i, j, COLOR_4C);
			else
				buf_check(ptr, surf_height, i, j, COLOR_88);

	munmap(ptr, buf->surface[0].size);
}

static int opt_handler(int opt, int opt_index, void *data)
{
	switch (opt) {
	case 'd':
		dump_surface = true;
		break;
	case 'W':
		surfwidth = atoi(optarg);
		break;
	case 'H':
		surfheight = atoi(optarg);
		break;
	case 'X':
		start_x = atoi(optarg);
		break;
	case 'Y':
		start_y = atoi(optarg);
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}


const char *help_str =
	"  -d\tDump surface\n"
	"  -W\tWidth (default 64)\n"
	"  -H\tHeight (default 64)\n"
	"  -X\tX start (aligned to 4)\n"
	"  -Y\tY start (aligned to 1)\n"
	;


igt_main_args("dW:H:X:Y:", NULL, help_str, opt_handler, NULL)
{
	data_t data = {0, };
	igt_fillfunc_t fill_fn = NULL;

	igt_fixture() {
		data.drm_fd = drm_open_driver_render(DRIVER_XE);
		data.devid = intel_get_drm_devid(data.drm_fd);
		data.bops = buf_ops_create(data.drm_fd);

		fill_fn = igt_get_gpgpu_fillfunc(data.devid);
		igt_require_f(fill_fn, "no gpgpu-fill function\n");

		start_x = ALIGN(start_x, 4);
	}

	igt_subtest("basic") {
		gpgpu_fill(&data, fill_fn, 0,
			   surfwidth, surfheight,
			   start_x, start_y,
			   surfwidth / 2,
			   surfheight / 2);
	}

	igt_subtest("offset-16x16") {
		gpgpu_fill(&data, fill_fn, 0,
			   surfwidth, surfheight,
			   16, 16,
			   surfwidth / 2,
			   surfheight / 2);
	}

	igt_fixture() {
		buf_ops_destroy(data.bops);
		drm_close_driver(data.drm_fd);
	}
}
