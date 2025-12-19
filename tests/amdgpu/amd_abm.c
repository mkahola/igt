/*
 * Copyright 2018 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "igt.h"
#include "drmtest.h"
#include "igt_kms.h"
#include <limits.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>

#define DEBUGFS_CURRENT_BACKLIGHT_PWM "amdgpu_current_backlight_pwm"
#define DEBUGFS_TARGET_BACKLIGHT_PWM "amdgpu_target_backlight_pwm"
#define BACKLIGHT_PATH "/sys/class/backlight/amdgpu_bl0"
#define PANEL_POWER_SAVINGS_PATH "/sys/class/drm/card0-%s/amdgpu/panel_power_savings"
#define MK_COLOR(r, g, b)	((0 << 24) | (r << 16) | (g << 8) | b)

typedef struct data {
	igt_display_t display;
	igt_plane_t *primary;
	igt_output_t *output;
	igt_crtc_t *pipe;
	int drm_fd;
	drmModeModeInfo *mode;
	enum pipe pipe_id;
	int w, h;
	igt_fb_t ref_fb;
	igt_fb_t ref_fb2;
	uint32_t *fb_mem;
} data_t;

static void set_abm_level(data_t *data, igt_output_t *output, int level);

static void fbmem_draw_smpte_pattern(uint32_t *fbmem, int width, int height)
{
	uint32_t x, y;
	uint32_t colors_top[] = {
		MK_COLOR(192, 192, 192), /* grey */
		MK_COLOR(192, 192, 0),   /* yellow */
		MK_COLOR(0, 192, 192),   /* cyan */
		MK_COLOR(0, 192, 0),     /* green */
		MK_COLOR(192, 0, 192),   /* magenta */
		MK_COLOR(192, 0, 0),     /* red */
		MK_COLOR(0, 0, 192),     /* blue */
	};
	uint32_t colors_middle[] = {
		MK_COLOR(0, 0, 192),     /* blue */
		MK_COLOR(19, 19, 19),    /* black */
		MK_COLOR(192, 0, 192),   /* magenta */
		MK_COLOR(19, 19, 19),    /* black */
		MK_COLOR(0, 192, 192),   /* cyan */
		MK_COLOR(19, 19, 19),    /* black */
		MK_COLOR(192, 192, 192), /* grey */
	};
	uint32_t colors_bottom[] = {
		MK_COLOR(0, 33, 76),     /* in-phase */
		MK_COLOR(255, 255, 255), /* super white */
		MK_COLOR(50, 0, 106),    /* quadrature */
		MK_COLOR(19, 19, 19),    /* black */
		MK_COLOR(9, 9, 9),       /* 3.5% */
		MK_COLOR(19, 19, 19),    /* 7.5% */
		MK_COLOR(29, 29, 29),    /* 11.5% */
		MK_COLOR(19, 19, 19),    /* black */
	};

	for (y = 0; y < height * 6 / 9; ++y) {
		for (x = 0; x < width; ++x)
			fbmem[x] = colors_top[x * 7 / width];
		fbmem += width;
	}

	for (; y < height * 7 / 9; ++y) {
		for (x = 0; x < width; ++x)
			fbmem[x] = colors_middle[x * 7 / width];
		fbmem += width;
	}

	for (; y < height; ++y) {
		for (x = 0; x < width * 5 / 7; ++x)
			fbmem[x] = colors_bottom[x * 4 / (width * 5 / 7)];
		for (; x < width * 6 / 7; ++x)
			fbmem[x] = colors_bottom[(x - width * 5 / 7) * 3 / (width / 7) + 4];
		for (; x < width; ++x)
			fbmem[x] = colors_bottom[7];
		fbmem += width;
	}
}

/* Common test setup. */
static void test_init(data_t *data)
{
	igt_display_t *display = &data->display;
	drmModeConnectorPtr conn;
	bool has_edp = false;
	int i, fb_id;

	/* Skip test if no eDP connected. */
	for (i = 0; i < display->n_outputs; i++) {
		conn = display->outputs[i].config.connector;

		if (conn->connector_type == DRM_MODE_CONNECTOR_eDP &&
		    conn->connection == DRM_MODE_CONNECTED) {
			has_edp = true;
		}
	}
	if (!has_edp)
		igt_skip("No eDP connector found\n");

	/* It doesn't matter which pipe we choose on amdpgu. */
	data->pipe_id = PIPE_A;
	data->pipe = igt_crtc_for_pipe(&data->display, data->pipe_id);

	igt_display_reset(display);

	data->output = igt_get_single_output_for_pipe(display, data->pipe_id);
	igt_require(data->output);
	igt_info("output %s\n", data->output->name);

	data->mode = igt_output_get_mode(data->output);
	igt_assert(data->mode);
	kmstest_dump_mode(data->mode);

	data->primary =
		 igt_crtc_get_plane_type(data->pipe, DRM_PLANE_TYPE_PRIMARY);

	igt_output_set_crtc(data->output,
			    igt_crtc_for_pipe(data->output->display, data->pipe_id));

	data->w = data->mode->hdisplay;
	data->h = data->mode->vdisplay;

	data->ref_fb.fb_id = 0;
	data->ref_fb2.fb_id = 0;

	fb_id = igt_create_fb(data->drm_fd, data->mode->hdisplay, data->mode->vdisplay,
		DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, &data->ref_fb);
	igt_assert(fb_id);

	data->fb_mem = igt_fb_map_buffer(data->drm_fd, &data->ref_fb);
	fbmem_draw_smpte_pattern(data->fb_mem, data->w, data->h);
	igt_create_color_fb(data->drm_fd, data->mode->hdisplay,
		data->mode->vdisplay, DRM_FORMAT_XRGB8888, 0, 0.05, 0.05, 0.05, &data->ref_fb2);
}

/* Common test cleanup. */
static void test_fini(data_t *data)
{
	igt_display_t *display = &data->display;
	igt_output_t *output;
	enum pipe pipe;

	/* Disable ABM before exit test */
	for_each_valid_output_on_pipe(&data->display, pipe, output) {
		if (output->config.connector->connector_type != DRM_MODE_CONNECTOR_eDP)
			continue;
		set_abm_level(data, output, 0);
	}

	igt_display_reset(display);
	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, 0);

	igt_fb_unmap_buffer(&data->ref_fb, data->fb_mem);
	if (data->ref_fb.fb_id)
		igt_remove_fb(data->drm_fd, &data->ref_fb);
	if (data->ref_fb2.fb_id)
		igt_remove_fb(data->drm_fd, &data->ref_fb2);
}


static int read_current_backlight_pwm(int drm_fd, char *connector_name)
{
	char buf[20];
	int fd;

	fd = igt_debugfs_connector_dir(drm_fd, connector_name, O_RDONLY);

	if (fd < 0) {
		igt_info("Couldn't open connector %s debugfs directory\n",
			 connector_name);
		return false;
	}

	igt_debugfs_simple_read(fd, DEBUGFS_CURRENT_BACKLIGHT_PWM, buf, sizeof(buf));
	close(fd);

	return strtol(buf, NULL, 0);
}

static int read_target_backlight_pwm(int drm_fd, char *connector_name)
{
	char buf[20];
	int fd;

	fd = igt_debugfs_connector_dir(drm_fd, connector_name, O_RDONLY);

	if (fd < 0) {
		igt_info("Couldn't open connector %s debugfs directory\n",
			 connector_name);
		return false;
	}

	igt_debugfs_simple_read(fd, DEBUGFS_TARGET_BACKLIGHT_PWM, buf, sizeof(buf));
	close(fd);

	return strtol(buf, NULL, 0);
}

static int backlight_write_brightness(int value)
{
	int fd;
	char full[PATH_MAX];
	char src[64];
	int len;

	igt_assert(snprintf(full, PATH_MAX, "%s/%s", BACKLIGHT_PATH, "brightness") < PATH_MAX);
	fd = open(full, O_WRONLY);
	if (fd == -1)
		return -errno;

	len = snprintf(src, sizeof(src), "%i", value);
	len = write(fd, src, len);
	close(fd);

	if (len < 0)
		return len;

	return 0;
}

static void set_abm_level(data_t *data, igt_output_t *output, int level)
{
	char buf[PATH_MAX];
	int fd;

	igt_assert(snprintf(buf, PATH_MAX, PANEL_POWER_SAVINGS_PATH,
			    output->name) < PATH_MAX);

	fd = open(buf, O_WRONLY);

	igt_skip_on_f(fd == -1, "Cannot find %s. Is it an OLED?\n", buf);

	igt_assert_eq(snprintf(buf, sizeof(buf), "%d", level),
		      write(fd, buf, 1));

	igt_assert_eq(close(fd), 0);

	igt_output_set_crtc(data->output,
			    igt_crtc_for_pipe(data->output->display, data->pipe_id));
	igt_plane_set_fb(data->primary, &data->ref_fb);
	igt_display_commit_atomic(&data->display, 0, 0);
}

static int backlight_read_max_brightness(int *result)
{
	int fd;
	char full[PATH_MAX];
	char dst[64];
	int r, e;

	igt_assert(snprintf(full, PATH_MAX, "%s/%s", BACKLIGHT_PATH, "max_brightness") < PATH_MAX);

	fd = open(full, O_RDONLY);
	if (fd == -1)
		return -errno;

	r = read(fd, dst, sizeof(dst));
	e = errno;
	close(fd);

	if (r < 0)
		return -e;

	errno = 0;
	*result = strtol(dst, NULL, 10);
	return errno;
}

static void page_flip(data_t *data, igt_output_t *output, uint32_t frame_num)
{
	int i, ret, frame_count;
	igt_fb_t *flip_fb;

	if (!data || data->ref_fb.fb_id == 0 || data->ref_fb2.fb_id == 0)
		igt_skip("Page flip failed.\n");

	for (i = 0; i < 2; i++) {
		if (i % 2 == 0)
			flip_fb = &data->ref_fb2;
		else
			flip_fb = &data->ref_fb;

		for (frame_count = 0; frame_count <= frame_num; frame_count++) {
			ret = drmModePageFlip(data->drm_fd, output->config.crtc->crtc_id,
					flip_fb->fb_id, DRM_MODE_PAGE_FLIP_EVENT, NULL);
			igt_require(ret == 0);
			kmstest_wait_for_pageflip(data->drm_fd);
		}
	}
}

static void backlight_dpms_cycle(data_t *data)
{
	int ret;
	int max_brightness;
	int pwm_1, pwm_2;
	igt_output_t *output;
	enum pipe pipe;

	for_each_valid_output_on_pipe(&data->display, pipe, output) {
		if (output->config.connector->connector_type != DRM_MODE_CONNECTOR_eDP)
			continue;

		igt_info("Testing backlight dpms on %s\n", output->name);

		ret = backlight_read_max_brightness(&max_brightness);
		igt_assert_eq(ret, 0);

		set_abm_level(data, output, 0);
		backlight_write_brightness(max_brightness / 2);
		usleep(100000);
		pwm_1 = read_target_backlight_pwm(data->drm_fd, output->name);

		kmstest_set_connector_dpms(data->drm_fd, output->config.connector, DRM_MODE_DPMS_OFF);
		kmstest_set_connector_dpms(data->drm_fd, output->config.connector, DRM_MODE_DPMS_ON);
		usleep(100000);
		pwm_2 = read_target_backlight_pwm(data->drm_fd, output->name);
		igt_assert_eq(pwm_1, pwm_2);
	}
}

static void backlight_monotonic_basic(data_t *data)
{
	int ret;
	int max_brightness;
	int prev_pwm, pwm;
	int brightness_step;
	int brightness;
	enum pipe pipe;
	igt_output_t *output;

	for_each_valid_output_on_pipe(&data->display, pipe, output) {
		if (output->config.connector->connector_type != DRM_MODE_CONNECTOR_eDP)
			continue;
		ret = backlight_read_max_brightness(&max_brightness);
		igt_assert_eq(ret, 0);

		brightness_step = max_brightness / 10;

		set_abm_level(data, output, 0);
		backlight_write_brightness(max_brightness);
		usleep(100000);
		prev_pwm = read_target_backlight_pwm(data->drm_fd, output->name);
		for (brightness = max_brightness - brightness_step;
			brightness > 0;
			brightness -= brightness_step) {
			backlight_write_brightness(brightness);
			usleep(100000);
			pwm = read_target_backlight_pwm(data->drm_fd, output->name);
			igt_assert(pwm < prev_pwm);
			prev_pwm = pwm;
		}
	}
}

static void backlight_monotonic_abm(data_t *data)
{
	int ret, i;
	int max_brightness;
	int prev_pwm, pwm;
	int brightness_step;
	int brightness;
	enum pipe pipe;
	igt_output_t *output;

	for_each_valid_output_on_pipe(&data->display, pipe, output) {
		if (output->config.connector->connector_type != DRM_MODE_CONNECTOR_eDP)
			continue;
		ret = backlight_read_max_brightness(&max_brightness);
		igt_assert_eq(ret, 0);

		brightness_step = max_brightness / 10;
		for (i = 1; i < 5; i++) {
			set_abm_level(data, output, i);
			backlight_write_brightness(max_brightness);
			usleep(100000);
			prev_pwm = read_target_backlight_pwm(data->drm_fd, output->name);
			for (brightness = max_brightness - brightness_step;
				brightness > 0;
				brightness -= brightness_step) {
				backlight_write_brightness(brightness);
				usleep(100000);
				pwm = read_target_backlight_pwm(data->drm_fd, output->name);
				igt_assert(pwm < prev_pwm);
				prev_pwm = pwm;
			}
		}
	}
}

static void abm_enabled(data_t *data)
{
	int ret, i;
	int max_brightness;
	int pwm, prev_pwm, pwm_without_abm;
	enum pipe pipe;
	igt_output_t *output;

	for_each_valid_output_on_pipe(&data->display, pipe, output) {
		if (output->config.connector->connector_type != DRM_MODE_CONNECTOR_eDP)
			continue;

		ret = backlight_read_max_brightness(&max_brightness);
		igt_assert_eq(ret, 0);

		set_abm_level(data, output, 0);
		backlight_write_brightness(max_brightness-max_brightness/10);
		usleep(100000);
		prev_pwm = read_target_backlight_pwm(data->drm_fd, output->name);
		pwm_without_abm = prev_pwm;

		for (i = 1; i < 5; i++) {
			set_abm_level(data, output, i);
			usleep(100000);
			page_flip(data, output, 10);
			pwm = read_target_backlight_pwm(data->drm_fd, output->name);
			igt_assert(pwm <= prev_pwm);
			igt_assert(pwm < pwm_without_abm);
			prev_pwm = pwm;
		}
	}
}

static void abm_gradual(data_t *data)
{
	int ret, i;
	int convergence_delay = 10;
	int prev_pwm, pwm, curr;
	int max_brightness;
	enum pipe pipe;
	igt_output_t *output;

	for_each_valid_output_on_pipe(&data->display, pipe, output) {
		if (output->config.connector->connector_type != DRM_MODE_CONNECTOR_eDP)
			continue;

		ret = backlight_read_max_brightness(&max_brightness);

		igt_assert_eq(ret, 0);

		set_abm_level(data, output, 0);
		backlight_write_brightness(max_brightness-max_brightness/10);

		sleep(convergence_delay);
		prev_pwm = read_target_backlight_pwm(data->drm_fd, output->name);
		curr = read_current_backlight_pwm(data->drm_fd, output->name);

		igt_assert_eq(prev_pwm, curr);
		set_abm_level(data, output, 4);
		for (i = 0; i < 10; i++) {
			usleep(100000);
			page_flip(data, output, 10);
			pwm = read_current_backlight_pwm(data->drm_fd, output->name);
			if (pwm == prev_pwm)
				break;
			igt_assert(pwm < prev_pwm);
			prev_pwm = pwm;
		}

		if (i < 10)
			igt_assert(i);
		else {
			sleep(convergence_delay - 1);
			prev_pwm = read_target_backlight_pwm(data->drm_fd, output->name);
			curr = read_current_backlight_pwm(data->drm_fd, output->name);
			igt_assert_eq(prev_pwm, curr);
		}
	}
}

int igt_main()
{
	data_t data = {0};
	igt_skip_on_simulation();

	igt_fixture() {
		data.drm_fd = drm_open_driver_master(DRIVER_AMDGPU);

		if (data.drm_fd == -1)
			igt_skip("Not an amdgpu driver.\n");

		kmstest_set_vt_graphics_mode();

		igt_display_require(&data.display, data.drm_fd);

		test_init(&data);
	}

	igt_subtest("dpms_cycle")
		backlight_dpms_cycle(&data);
	igt_subtest("backlight_monotonic_basic")
		backlight_monotonic_basic(&data);
	igt_subtest("backlight_monotonic_abm")
		backlight_monotonic_abm(&data);
	igt_subtest("abm_enabled")
		abm_enabled(&data);
	igt_subtest("abm_gradual")
		abm_gradual(&data);

	igt_fixture() {
		test_fini(&data);
		igt_display_fini(&data.display);
		drm_close_driver(data.drm_fd);
	}
}
