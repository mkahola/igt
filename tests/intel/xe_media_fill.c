// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

/*
 * This file is a basic test for the media_fill() function, a very simple
 * workload for the Media pipeline.
 */

#include <stdbool.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "drm.h"
#include "igt.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"

/**
 * TEST: xe media fill
 * Category: Core
 * Mega feature: Media
 * Sub-category: Media tests
 * Functionality: fill surface with media block
 * Description: Basic tests for the media_fill() function.
 * Feature: media
 * Test category: functionality test
 *
 * SUBTEST: media-fill
 * Description: Basic test for the media_fill() function,
 *              a very simple workload for the Media pipeline.
 */

IGT_TEST_DESCRIPTION("Basic test for the media_fill() function, a very simple"
		     " xe workload for the Media pipeline.");

#define WIDTH 64
#define STRIDE (WIDTH)
#define HEIGHT 64
#define SIZE (HEIGHT*STRIDE)

#define COLOR_C4	0xc4
#define COLOR_4C	0x4c

struct data_t {
	int drm_fd;
	uint32_t devid;
	struct buf_ops *bops;
};

static struct intel_buf *
create_buf(struct data_t *data, int width, int height, uint8_t color)
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

static void buf_check(uint8_t *ptr, int x, int y, uint8_t color)
{
	uint8_t val;

	val = ptr[y * WIDTH + x];
	igt_assert_f(val == color,
		     "Expected 0x%02x, found 0x%02x at (%d,%d)\n",
		     color, val, x, y);
}

static void media_fill(struct data_t *data, igt_fillfunc_t fill)
{
	struct intel_buf *buf;
	uint8_t *ptr;
	int i, j;

	buf = create_buf(data, WIDTH, HEIGHT, COLOR_C4);
	ptr = xe_bo_map(data->drm_fd, buf->handle, buf->surface[0].size);

	for (i = 0; i < WIDTH; i++)
		for (j = 0; j < HEIGHT; j++)
			buf_check(ptr, i, j, COLOR_C4);

	fill(data->drm_fd, buf, 0, 0, WIDTH / 2, HEIGHT / 2, COLOR_4C);

	for (i = 0; i < WIDTH; i++)
		for (j = 0; j < HEIGHT; j++)
			if (i < WIDTH / 2 && j < HEIGHT / 2)
				buf_check(ptr, i, j, COLOR_4C);
			else
				buf_check(ptr, i, j, COLOR_C4);

	munmap(ptr, buf->surface[0].size);
}

igt_main()
{
	struct data_t data = {0, };
	igt_fillfunc_t fill_fn = NULL;

	igt_fixture() {
		data.drm_fd = drm_open_driver_render(DRIVER_XE);
		data.devid = intel_get_drm_devid(data.drm_fd);
		data.bops = buf_ops_create(data.drm_fd);

		fill_fn = igt_get_media_fillfunc(data.devid);

		igt_require_f(fill_fn, "no media-fill function\n");
	}

	igt_subtest("media-fill")
		media_fill(&data, fill_fn);

	igt_fixture() {
		buf_ops_destroy(data.bops);
		drm_close_driver(data.drm_fd);
	}
}

