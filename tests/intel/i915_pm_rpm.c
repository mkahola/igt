/*
 * Copyright © 2013, 2015 Intel Corporation
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
 *    Paulo Zanoni <paulo.r.zanoni@intel.com>
 *
 */

#include "config.h"

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ftw.h>

#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
/**
 * TEST: i915 pm rpm
 * Category: Core
 * Mega feature: Power management
 * Sub-category: Power management tests
 * Functionality: D3 state entry-exit
 * Test category: pm_rpm
 * Feature: pm_rpm
 *
 * SUBTEST: debugfs-forcewake-user
 * SUBTEST: debugfs-read
 * SUBTEST: gem-evict-pwrite
 * SUBTEST: gem-execbuf
 *
 * SUBTEST: gem-execbuf-stress
 * Description: Validate execbuf submission while exercising rpm suspend/resume cycles.
 *
 * SUBTEST: gem-execbuf-stress-pc8
 * SUBTEST: gem-idle
 * SUBTEST: gem-mmap-type
 * SUBTEST: gem-pread
 * SUBTEST: module-reload
 * SUBTEST: reg-read-ioctl
 * SUBTEST: sysfs-read
 * SUBTEST: system-hibernate
 * SUBTEST: system-hibernate-devices
 * SUBTEST: system-suspend
 * SUBTEST: system-suspend-devices
 * SUBTEST: system-suspend-execbuf
 */

#if defined(__linux__)
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#elif defined(__FreeBSD__)
#include <dev/iicbus/iic.h>
#define	addr	slave
#endif

#include <drm.h>

#include "i915/gem.h"
#include "i915/gem_create.h"
#include "igt.h"
#include "igt_kmod.h"
#include "igt_sysfs.h"
#include "igt_debugfs.h"
#include "igt_device.h"
#include "igt_edid.h"
#include "intel_blt.h"

#define MSR_PC8_RES	0x630
#define MSR_PC9_RES	0x631
#define MSR_PC10_RES	0x632

#define MAX_CONNECTORS	32
#define MAX_ENCODERS	32
#define MAX_CRTCS	16

#define WIDTH 64
#define HEIGHT 64
#define STRIDE (WIDTH)
#define SIZE (HEIGHT * STRIDE)

enum pc8_status {
	PC8_ENABLED,
	PC8_DISABLED
};

enum screen_type {
	SCREEN_TYPE_LPSP,
	SCREEN_TYPE_NON_LPSP,
	SCREEN_TYPE_ANY,
};

enum plane_type {
	PLANE_OVERLAY,
	PLANE_PRIMARY,
	PLANE_CURSOR,
};

/* Wait flags */
#define DONT_WAIT	0
#define WAIT_STATUS	1
#define WAIT_PC8_RES	2
#define WAIT_EXTRA	4
#define USE_DPMS	8

int drm_fd, msr_fd, pc8_status_fd;
int debugfs;
bool has_runtime_pm, has_pc8;
struct mode_set_data ms_data;

/* Stuff used when creating FBs and mode setting. */
struct mode_set_data {
	drmModeResPtr res;
	drmModeConnectorPtr connectors[MAX_CONNECTORS];
	drmModePropertyBlobPtr edids[MAX_CONNECTORS];
	igt_display_t display;

	uint32_t devid;
	int fw_fd;
};

/* Stuff we query at different times so we can compare. */
struct compare_data {
	drmModeResPtr res;
	drmModeEncoderPtr encoders[MAX_ENCODERS];
	drmModeConnectorPtr connectors[MAX_CONNECTORS];
	drmModeCrtcPtr crtcs[MAX_CRTCS];
	drmModePropertyBlobPtr edids[MAX_CONNECTORS];
};

struct modeset_params {
	uint32_t crtc_id;
	uint32_t connector_id;
	struct igt_fb fb;
	drmModeModeInfoPtr mode;
};

struct data_t {
	int width;
	int height;
	uint32_t region;
};

struct modeset_params lpsp_mode_params;
struct modeset_params non_lpsp_mode_params;
struct modeset_params *default_mode_params;

/* API to create mmap buffer */
static struct intel_buf *
create_buf(struct data_t *data, uint32_t color)
{
	struct intel_buf *buf;
	uint8_t *ptr;
	struct buf_ops *bops;
	int i;

	buf = calloc(1, sizeof(*buf));
	igt_assert(buf);
	bops = buf_ops_create(drm_fd);

	intel_buf_init_in_region(bops, buf,
				 data->width / 4, data->height, 32, 0,
				 I915_TILING_NONE, 0, data->region);

	ptr = gem_mmap__cpu_coherent(drm_fd, buf->handle, 0,
				     buf->surface[0].size, PROT_WRITE);

	for (i = 0; i < buf->surface[0].size; i++)
		ptr[i] = color;

	munmap(ptr, buf->surface[0].size);

	return buf;
}

/* checking the buffer content is correct or not */
static void buf_check(uint8_t *ptr, int x, int y, uint8_t color)
{
	uint8_t val;

	val = ptr[y * WIDTH + x];
	igt_assert_f(val == color,
		     "Expected 0x%02x, found 0x%02x at (%d,%d)\n",
		     color, val, x, y);
}

static int modprobe(const char *driver)
{
	return igt_kmod_load(driver, NULL);
}

/* If the read fails, then the machine doesn't support PC8+ residencies. */
static bool supports_pc8_plus_residencies(void)
{
	int rc;
	uint64_t val;

	rc = pread(msr_fd, &val, sizeof(uint64_t), MSR_PC8_RES);
	if (rc != sizeof(val))
		return false;
	rc = pread(msr_fd, &val, sizeof(uint64_t), MSR_PC9_RES);
	if (rc != sizeof(val))
		return false;
	rc = pread(msr_fd, &val, sizeof(uint64_t), MSR_PC10_RES);
	if (rc != sizeof(val))
		return false;

	return igt_pm_pc8_plus_residencies_enabled(msr_fd);
}

static uint64_t get_residency(uint32_t type)
{
	int rc;
	uint64_t ret;

	rc = pread(msr_fd, &ret, sizeof(uint64_t), type);
	igt_assert(rc == sizeof(ret));

	return ret;
}

static bool pc8_plus_residency_changed(unsigned int timeout_sec)
{
	uint64_t res_pc8, res_pc9, res_pc10;

	res_pc8 = get_residency(MSR_PC8_RES);
	res_pc9 = get_residency(MSR_PC9_RES);
	res_pc10 = get_residency(MSR_PC10_RES);

	return igt_wait(res_pc8 != get_residency(MSR_PC8_RES) ||
			res_pc9 != get_residency(MSR_PC9_RES) ||
			res_pc10 != get_residency(MSR_PC10_RES),
			timeout_sec * 1000, 100);
}

static enum pc8_status get_pc8_status(void)
{
	ssize_t n_read;
	char buf[150]; /* The whole file has less than 100 chars. */

	lseek(pc8_status_fd, 0, SEEK_SET);
	n_read = read(pc8_status_fd, buf, ARRAY_SIZE(buf));
	igt_assert(n_read >= 0);
	buf[n_read] = '\0';

	if (strstr(buf, "\nEnabled: yes\n"))
		return PC8_ENABLED;
	else
		return PC8_DISABLED;
}

static bool is_suspended(void)
{
	if (has_pc8 && !has_runtime_pm)
		return get_pc8_status() == PC8_ENABLED;
	else
		return igt_get_runtime_pm_status() == IGT_RUNTIME_PM_STATUS_SUSPENDED;
}

static bool wait_for_pc8_status(enum pc8_status status)
{
	return igt_wait(get_pc8_status() == status, 10000, 100);
}

static bool wait_for_suspended(void)
{
	if (has_pc8 && !has_runtime_pm) {
		return wait_for_pc8_status(PC8_ENABLED);
	} else {
		bool suspended = igt_wait_for_pm_status(IGT_RUNTIME_PM_STATUS_SUSPENDED);

		if (!suspended) {
			/* Dump runtime pm status even if test skips */
			__igt_debugfs_dump(drm_fd, "i915_runtime_pm_status", IGT_LOG_INFO);
		}

		return suspended;
	}
}

static bool wait_for_active(void)
{
	if (has_pc8 && !has_runtime_pm)
		return wait_for_pc8_status(PC8_DISABLED);
	else
		return igt_wait_for_pm_status(IGT_RUNTIME_PM_STATUS_ACTIVE);
}

static void disable_all_screens_dpms(struct mode_set_data *data)
{
	if (!data->res)
		return;

	for (int i = 0; i < data->res->count_connectors; i++) {
		drmModeConnectorPtr c = data->connectors[i];

		kmstest_set_connector_dpms(drm_fd, c, DRM_MODE_DPMS_OFF);
	}
}

static void disable_all_screens(struct mode_set_data *data)
{
	if (data->res)
		kmstest_unset_all_crtcs(drm_fd, data->res);
}

#define disable_all_screens_and_wait(data) do { \
	disable_all_screens(data); \
	igt_assert(wait_for_suspended()); \
} while (0)

static void disable_or_dpms_all_screens(struct mode_set_data *data, bool dpms)
{
	if (dpms)
		disable_all_screens_dpms(&ms_data);
	else
		disable_all_screens(&ms_data);
}

#define disable_or_dpms_all_screens_and_wait(data, dpms) do { \
	disable_or_dpms_all_screens((data), (dpms)); \
	igt_assert(wait_for_suspended()); \
} while (0)

static bool init_modeset_params_for_type(struct mode_set_data *data,
					 struct modeset_params *params,
					 enum screen_type type)
{
	drmModeConnectorPtr connector = NULL;
	drmModeModeInfoPtr mode = NULL;
	igt_output_t *output = NULL;
	igt_display_t *display = &data->display;

	if (!data->res || !display)
		return false;

	for_each_connected_output(display, output) {
		drmModeConnectorPtr c = output->config.connector;

		if (type == SCREEN_TYPE_LPSP &&
		     !i915_output_is_lpsp_capable(drm_fd, output))
			continue;

		if (type == SCREEN_TYPE_NON_LPSP &&
		    i915_output_is_lpsp_capable(drm_fd, output))
			continue;

		connector = c;
		mode = igt_output_get_mode(output);
		break;
	}

	if (!connector || !mode)
		return false;

	igt_create_pattern_fb(drm_fd, mode->hdisplay, mode->vdisplay,
			      DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
			      &params->fb);

	params->crtc_id = kmstest_find_crtc_for_connector(drm_fd, data->res,
							  connector, 0);
	params->connector_id = connector->connector_id;
	params->mode = mode;

	return true;
}

static void init_modeset_cached_params(struct mode_set_data *data)
{
	bool lpsp, non_lpsp;

	lpsp = init_modeset_params_for_type(data, &lpsp_mode_params,
					    SCREEN_TYPE_LPSP);
	non_lpsp = init_modeset_params_for_type(data, &non_lpsp_mode_params,
						SCREEN_TYPE_NON_LPSP);

	if (lpsp)
		default_mode_params = &lpsp_mode_params;
	else if (non_lpsp)
		default_mode_params = &non_lpsp_mode_params;
	else
		default_mode_params = NULL;
}

static bool set_mode_for_params(struct modeset_params *params)
{
	int rc;

	rc = drmModeSetCrtc(drm_fd, params->crtc_id, params->fb.fb_id, 0, 0,
			    &params->connector_id, 1, params->mode);
	return (rc == 0);
}

#define set_mode_for_params_and_wait(params) do { \
	igt_assert(set_mode_for_params(params)); \
	igt_assert(wait_for_active()); \
} while (0)

static bool enable_one_screen_with_type(struct mode_set_data *data,
					enum screen_type type)
{
	struct modeset_params *params = NULL;

	switch (type) {
	case SCREEN_TYPE_ANY:
		params = default_mode_params;
		break;
	case SCREEN_TYPE_LPSP:
		params = &lpsp_mode_params;
		break;
	case SCREEN_TYPE_NON_LPSP:
		params = &non_lpsp_mode_params;
		break;
	default:
		igt_assert(0);
	}

	if (!params)
		return false;

	return set_mode_for_params(params);
}

static void
enable_one_screen_or_forcewake_get_and_wait(struct mode_set_data *data)
{
	bool headless;

	/* Try to resume by enabling any type of display */
	headless = !enable_one_screen_with_type(data, SCREEN_TYPE_ANY);

	/*
	 * Get User Forcewake to trigger rpm resume in case of headless
	 * as well as no display being connected.
	 */
	if (headless) {
		data->fw_fd = igt_open_forcewake_handle(drm_fd);
		igt_require(data->fw_fd > 0);
	}
	igt_assert(wait_for_active());
}

static void forcewake_put(struct mode_set_data *data)
{
	if (data->fw_fd <= 0)
		return;

	data->fw_fd = close(data->fw_fd);
	igt_assert_eq(data->fw_fd, 0);
}

static void
disable_all_screens_or_forcewake_put_and_wait(struct mode_set_data *data)
{
	forcewake_put(data);
	disable_all_screens(data);
	igt_assert(wait_for_suspended());
}

static drmModePropertyBlobPtr get_connector_edid(drmModeConnectorPtr connector,
						 int index)
{
	bool found;
	uint64_t prop_value;
	drmModePropertyPtr prop;
	drmModePropertyBlobPtr blob = NULL;

	found = kmstest_get_property(drm_fd, connector->connector_id,
				     DRM_MODE_OBJECT_CONNECTOR, "EDID",
				     NULL, &prop_value, &prop);

	if (found) {
		igt_assert(prop->flags & DRM_MODE_PROP_BLOB);
		igt_assert(prop->count_blobs == 0);

		blob = drmModeGetPropertyBlob(drm_fd, prop_value);

		drmModeFreeProperty(prop);
	}

	return blob;
}

static bool init_mode_set_data(struct mode_set_data *data)
{
	data->res = drmModeGetResources(drm_fd);
	if (data->res) {
		igt_assert(data->res->count_connectors <= MAX_CONNECTORS);
		for (int i = 0; i < data->res->count_connectors; i++) {
			data->connectors[i] =
				drmModeGetConnector(drm_fd,
						    data->res->connectors[i]);
			if (!data->connectors[i]) {
				igt_warn("Could not read connector %u\n",
					 data->res->connectors[i]);
				return false;
			}

			data->edids[i] = get_connector_edid(data->connectors[i], i);
		}

		kmstest_set_vt_graphics_mode();
		igt_display_require(&data->display, drm_fd);
	}

	init_modeset_cached_params(&ms_data);

	return true;
}

static void fini_mode_set_data(struct mode_set_data *data)
{
	if (data->res) {
		for (int i = 0; i < data->res->count_connectors; i++) {
			drmModeFreeConnector(data->connectors[i]);
			drmModeFreePropertyBlob(data->edids[i]);
		}
		drmModeFreeResources(data->res);
		igt_display_fini(&data->display);
	}
}

static void setup_pc8(void)
{
	has_pc8 = false;

	/* Only Haswell supports the PC8 feature. */
	if (!IS_HASWELL(ms_data.devid) && !IS_BROADWELL(ms_data.devid))
		return;

	/* Make sure our Kernel supports MSR and the module is loaded. */
	igt_require(modprobe("msr") == 0);

	msr_fd = open("/dev/cpu/0/msr", O_RDONLY);
	igt_assert_f(msr_fd >= 0,
		     "Can't open /dev/cpu/0/msr.\n");

	/* Non-ULT machines don't support PC8+. */
	if (!supports_pc8_plus_residencies())
		return;

	pc8_status_fd = openat(debugfs, "i915_pc8_status", O_RDONLY);
	if (pc8_status_fd == -1)
		pc8_status_fd = openat(debugfs,
				       "i915_runtime_pm_status", O_RDONLY);
	igt_assert_f(pc8_status_fd >= 0,
		     "Can't open /sys/kernel/debug/dri/0/i915_runtime_pm_status");

	has_pc8 = true;
}

static void dump_file(int dir, const char *filename)
{
	char *contents;

	contents = igt_sysfs_get(dir, filename);
	if (!contents)
		return;

	igt_info("%s:\n%s\n", filename, contents);
	free(contents);
}

static bool setup_environment(bool display_enabled)
{
	if (has_runtime_pm)
		goto out;

	drm_fd = __drm_open_driver(DRIVER_INTEL);
	igt_require(drm_fd != -1);
	igt_device_set_master(drm_fd);

	debugfs = igt_debugfs_dir(drm_fd);
	igt_require(debugfs != -1);

	ms_data.devid = intel_get_drm_devid(drm_fd);

	/*
	 * Fail test requirements if we can't set the initial data.
	 * This is usually caused by not being able to retrieve a
	 * connector due to possible race-conditions.  In this case,
	 * we should have issued a warning to cause the test to fail
	 * and not just skip.
	 */
	if (display_enabled && !init_mode_set_data(&ms_data))
		return false;

	igt_pm_enable_sata_link_power_management();

	has_runtime_pm = igt_setup_runtime_pm(drm_fd);
	setup_pc8();

	igt_info("Runtime PM support: %d\n", has_runtime_pm);
	igt_info("PC8 residency support: %d\n", has_pc8);
	igt_require(has_runtime_pm);
	igt_require(igt_pm_dmc_loaded(debugfs));

out:
	if (display_enabled)
		disable_all_screens(&ms_data);
	dump_file(debugfs, "i915_runtime_pm_status");

	return wait_for_suspended();
}

static void teardown_environment(bool display_enabled)
{
	close(msr_fd);
	if (has_pc8)
		close(pc8_status_fd);

	igt_restore_runtime_pm();

	igt_pm_restore_sata_link_power_management();

	if (display_enabled)
		fini_mode_set_data(&ms_data);

	close(debugfs);
	close(drm_fd);

	has_runtime_pm = false;
}

struct read_entry_elapsed {
	uint64_t elapsed;
	char *path;
} max_read_entry;

static int read_entry(const char *filepath,
		      const struct stat *info,
		      const int typeflag,
		      struct FTW *pathinfo)
{
	struct timespec tv = {};
	uint64_t elapsed;
	char buf[4096];
	int fd;
	int rc;

	igt_nsec_elapsed(&tv);

	igt_assert_f(is_suspended(), "Before opening: %s (%s)\n",
		     filepath + pathinfo->base, filepath);

	fd = open(filepath, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		igt_debug("Failed to open '%s': %m\n", filepath);
		return 0;
	}

	do {
		rc = read(fd, buf, sizeof(buf));
	} while (rc == sizeof(buf));

	close(fd);

	igt_drop_caches_set(drm_fd, DROP_IDLE); /* flush pm-idle */
	igt_assert_f(wait_for_suspended(), "After closing: %s (%s)\n",
		     filepath + pathinfo->base, filepath);

	elapsed = igt_nsec_elapsed(&tv);
	if (elapsed > max_read_entry.elapsed) {
		max_read_entry.elapsed = elapsed;
		free(max_read_entry.path);
		max_read_entry.path = strdup(filepath);
	}

	return 0;
}

static void walk_fs(char *path)
{
	max_read_entry.elapsed = 0;

	disable_all_screens_and_wait(&ms_data);
	nftw(path, read_entry, 20, FTW_PHYS | FTW_MOUNT);

	if (max_read_entry.path) {
		igt_info("Slowest file + suspend: %s took %.2fms\n",
			 max_read_entry.path, max_read_entry.elapsed * 1e-6);
		free(max_read_entry.path);
		max_read_entry.path = NULL;
	}
}

/* This test will probably pass, with a small chance of hanging the machine in
 * case of bugs. Many of the bugs exercised by this patch just result in dmesg
 * errors, so a "pass" here should be confirmed by a check on dmesg. */
static void debugfs_read_subtest(void)
{
	char path[256];

	igt_require_f(igt_debugfs_path(drm_fd, path, sizeof(path)),
		      "Can't find the debugfs directory\n");
	walk_fs(path);
}

/* Read the comment on debugfs_read_subtest(). */
static void sysfs_read_subtest(void)
{
	char path[80];

	igt_require_f(igt_sysfs_path(drm_fd, path, sizeof(path)),
		      "Can't find the sysfs directory\n");
	walk_fs(path);
}

/* Make sure we don't suspend when we have the i915_forcewake_user file open. */
static void debugfs_forcewake_user_subtest(void)
{
	int fd, rc;

	igt_require(intel_gen(ms_data.devid) >= 6);

	disable_all_screens_and_wait(&ms_data);

	fd = igt_open_forcewake_handle(drm_fd);
	igt_require(fd >= 0);

	if (has_runtime_pm) {
		igt_assert(wait_for_active());
		sleep(10);
		igt_assert(wait_for_active());
	} else {
		igt_assert(wait_for_suspended());
	}

	rc = close(fd);
	igt_assert_eq(rc, 0);

	igt_assert(wait_for_suspended());
}

static void gem_mmap_args(const struct mmap_offset *t,
			  struct drm_i915_gem_memory_class_instance *mem_regions)
{
	int i;
	uint32_t handle;
	int buf_size = 8192;
	uint8_t *gem_buf;

	/* Create, map and set data while the device is active. */
	enable_one_screen_or_forcewake_get_and_wait(&ms_data);

	handle = gem_create_in_memory_region_list(drm_fd, buf_size, 0, mem_regions, 1);

	gem_buf = __gem_mmap_offset(drm_fd, handle, 0, buf_size,
				    PROT_READ | PROT_WRITE, t->type);
	igt_require(gem_buf);

	for (i = 0; i < buf_size; i++)
		gem_buf[i] = i & 0xFF;

	for (i = 0; i < buf_size; i++)
		igt_assert(gem_buf[i] == (i & 0xFF));

	/* Now suspend, read and modify. */
	disable_all_screens_or_forcewake_put_and_wait(&ms_data);

	for (i = 0; i < buf_size; i++)
		igt_assert(gem_buf[i] == (i & 0xFF));
	igt_assert(wait_for_suspended());

	for (i = 0; i < buf_size; i++)
		gem_buf[i] = (~i & 0xFF);
	igt_assert(wait_for_suspended());

	/* Now resume and see if it's still there. */
	enable_one_screen_or_forcewake_get_and_wait(&ms_data);
	for (i = 0; i < buf_size; i++)
		igt_assert(gem_buf[i] == (~i & 0xFF));

	igt_assert(munmap(gem_buf, buf_size) == 0);

	/* Now the opposite: suspend, and try to create the mmap while
	 * suspended. */
	disable_all_screens_or_forcewake_put_and_wait(&ms_data);

	gem_buf = __gem_mmap_offset(drm_fd, handle, 0, buf_size,
				    PROT_READ | PROT_WRITE, t->type);
	igt_require(gem_buf);

	igt_assert(wait_for_suspended());

	for (i = 0; i < buf_size; i++)
		gem_buf[i] = i & 0xFF;

	for (i = 0; i < buf_size; i++)
		igt_assert(gem_buf[i] == (i & 0xFF));

	igt_assert(wait_for_suspended());

	/* Resume and check if it's still there. */
	enable_one_screen_or_forcewake_get_and_wait(&ms_data);
	for (i = 0; i < buf_size; i++)
		igt_assert(gem_buf[i] == (i & 0xFF));

	igt_assert(munmap(gem_buf, buf_size) == 0);
	gem_close(drm_fd, handle);
	forcewake_put(&ms_data);
}

static void gem_pread_subtest(void)
{
	int i;
	uint32_t handle;
	int buf_size = 8192;
	uint8_t *cpu_buf, *read_buf;

	cpu_buf = malloc(buf_size);
	read_buf = malloc(buf_size);
	igt_assert(cpu_buf);
	igt_assert(read_buf);
	memset(cpu_buf, 0, buf_size);
	memset(read_buf, 0, buf_size);

	/* Create and set data while the device is active. */
	enable_one_screen_or_forcewake_get_and_wait(&ms_data);

	handle = gem_create(drm_fd, buf_size);

	for (i = 0; i < buf_size; i++)
		cpu_buf[i] = i & 0xFF;

	gem_write(drm_fd, handle, 0, cpu_buf, buf_size);

	gem_read(drm_fd, handle, 0, read_buf, buf_size);

	for (i = 0; i < buf_size; i++)
		igt_assert(cpu_buf[i] == read_buf[i]);

	/* Now suspend, read and modify. */
	disable_all_screens_or_forcewake_put_and_wait(&ms_data);

	memset(read_buf, 0, buf_size);
	gem_read(drm_fd, handle, 0, read_buf, buf_size);

	for (i = 0; i < buf_size; i++)
		igt_assert(cpu_buf[i] == read_buf[i]);
	igt_assert(wait_for_suspended());

	for (i = 0; i < buf_size; i++)
		cpu_buf[i] = (~i & 0xFF);
	gem_write(drm_fd, handle, 0, cpu_buf, buf_size);
	igt_assert(wait_for_suspended());

	/* Now resume and see if it's still there. */
	enable_one_screen_or_forcewake_get_and_wait(&ms_data);

	memset(read_buf, 0, buf_size);
	gem_read(drm_fd, handle, 0, read_buf, buf_size);

	for (i = 0; i < buf_size; i++)
		igt_assert(cpu_buf[i] == read_buf[i]);

	gem_close(drm_fd, handle);

	free(cpu_buf);
	free(read_buf);
	forcewake_put(&ms_data);
}

/* Paints a square of color $color, size $width x $height, at position $x x $y
 * of $dst_handle, which contains pitch $pitch. */
static void submit_blt_cmd(uint32_t dst_handle, int dst_size,
			   uint16_t x, uint16_t y,
			   uint16_t width, uint16_t height, uint32_t pitch,
			   uint32_t color, uint32_t *presumed_dst_offset)
{
	int i, reloc_pos;
	uint32_t batch_handle;
	int batch_size = 8 * sizeof(uint32_t);
	uint32_t batch_buf[batch_size];
	bool cmd_extended;
	struct drm_i915_gem_execbuffer2 execbuf = {};
	struct drm_i915_gem_exec_object2 objs[2] = {{}, {}};
	struct drm_i915_gem_relocation_entry relocs[1] = {{}};
	struct drm_i915_gem_wait gem_wait;
	uint64_t ahnd = get_reloc_ahnd(drm_fd, 0), dst_offset;
	const struct intel_cmds_info *cmds_info = intel_get_cmds_info(ms_data.devid);

	if (ahnd)
		dst_offset = get_offset(ahnd, dst_handle, dst_size, 0);
	else
		dst_offset = *presumed_dst_offset;

	cmd_extended = blt_cmd_has_property(cmds_info, XY_COLOR_BLT,
					    BLT_CMD_EXTENDED);
	i = 0;

	if (cmd_extended)
		batch_buf[i++] = XY_COLOR_BLT_CMD_NOLEN |
				 XY_COLOR_BLT_WRITE_ALPHA |
				 XY_COLOR_BLT_WRITE_RGB | 0x5;
	else
		batch_buf[i++] = XY_COLOR_BLT_CMD_NOLEN |
				 XY_COLOR_BLT_WRITE_ALPHA |
				 XY_COLOR_BLT_WRITE_RGB | 0x4;
	batch_buf[i++] = (3 << 24) | (0xF0 << 16) | (pitch);
	batch_buf[i++] = (y << 16) | x;
	batch_buf[i++] = ((y + height) << 16) | (x + width);
	reloc_pos = i;
	batch_buf[i++] = dst_offset;
	if (cmd_extended)
		batch_buf[i++] = dst_offset >> 32;
	batch_buf[i++] = color;

	batch_buf[i++] = MI_BATCH_BUFFER_END;
	if (!cmd_extended)
		batch_buf[i++] = MI_NOOP;

	igt_assert(i * sizeof(uint32_t) == batch_size);

	batch_handle = gem_create(drm_fd, batch_size);
	gem_write(drm_fd, batch_handle, 0, batch_buf, batch_size);

	relocs[0].target_handle = dst_handle;
	relocs[0].delta = 0;
	relocs[0].offset = reloc_pos * sizeof(uint32_t);
	relocs[0].presumed_offset = *presumed_dst_offset;
	relocs[0].read_domains = 0;
	relocs[0].write_domain = I915_GEM_DOMAIN_RENDER;

	objs[0].handle = dst_handle;
	objs[0].alignment = 0;

	objs[1].handle = batch_handle;
	objs[1].relocation_count = !ahnd ? 1 : 0;
	objs[1].relocs_ptr = (uintptr_t)relocs;

	if (ahnd) {
		objs[0].offset = dst_offset;
		objs[0].flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_WRITE;
		objs[1].offset = get_offset(ahnd, batch_handle, batch_size, 0);
		objs[1].flags = EXEC_OBJECT_PINNED;
	}

	execbuf.buffers_ptr = (uintptr_t)objs;
	execbuf.buffer_count = 2;
	execbuf.batch_len = batch_size;
	execbuf.flags = I915_EXEC_BLT;
	i915_execbuffer2_set_context_id(execbuf, 0);

	gem_execbuf(drm_fd, &execbuf);

	*presumed_dst_offset = relocs[0].presumed_offset;

	gem_wait.flags = 0;
	gem_wait.timeout_ns = 10000000000LL; /* 10s */

	gem_wait.bo_handle = batch_handle;
	do_ioctl(drm_fd, DRM_IOCTL_I915_GEM_WAIT, &gem_wait);

	gem_wait.bo_handle = dst_handle;
	do_ioctl(drm_fd, DRM_IOCTL_I915_GEM_WAIT, &gem_wait);

	gem_close(drm_fd, batch_handle);
	put_ahnd(ahnd);
}

/* Make sure we can submit a batch buffer and verify its result. */
static void gem_execbuf_subtest(struct drm_i915_gem_memory_class_instance *mem_regions)
{
	int x, y, i, j;
	uint32_t handle;
	int bpp = 4;
	int pitch = 128 * bpp;
	int dst_size = 128 * 128 * bpp; /* 128x128 square */
	uint32_t *cpu_buf;
	uint32_t presumed_offset = 0;
	int sq_x = 5, sq_y = 10, sq_w = 15, sq_h = 20;
	uint32_t color;
	struct intel_buf *buf;
	uint8_t *ptr;
	struct data_t data = {0, };
	struct igt_collection *region;
	struct drm_i915_query_memory_regions *region_info;
	struct igt_collection *region_set;
	uint32_t id;

	igt_require_gem(drm_fd);
	gem_require_blitter(drm_fd);

	region_info = gem_get_query_memory_regions(drm_fd);
	igt_assert(region_info);
	region_set = get_memory_region_set(region_info,
					   I915_DEVICE_MEMORY,
					   I915_SYSTEM_MEMORY);
	for_each_combination(region, 1, region_set) {
		id = igt_collection_get_value(region, 0);
		break;
	}

	data.width = WIDTH;
	data.height = HEIGHT;
	data.region = id;

	/* Create and set data while the device is active. */
	enable_one_screen_or_forcewake_get_and_wait(&ms_data);

	handle = gem_create_in_memory_region_list(drm_fd, dst_size, 0, mem_regions, 1);

	cpu_buf = malloc(dst_size);
	igt_assert(cpu_buf);
	memset(cpu_buf, 0, dst_size);
	gem_write(drm_fd, handle, 0, cpu_buf, dst_size);

	/* Now suspend and try it. */
	disable_all_screens_or_forcewake_put_and_wait(&ms_data);

	color = 0x12345678;
	if (blt_has_xy_color(drm_fd)) {
		submit_blt_cmd(handle, dst_size, sq_x, sq_y, sq_w, sq_h, pitch, color,
			       &presumed_offset);
		igt_assert(wait_for_suspended());

		gem_read(drm_fd, handle, 0, cpu_buf, dst_size);
		for (y = 0; y < 128; y++) {
			for (x = 0; x < 128; x++) {
				uint32_t px = cpu_buf[y * 128 + x];

				if (y >= sq_y && y < (sq_y + sq_h) &&
				    x >= sq_x && x < (sq_x + sq_w))
					igt_assert_eq_u32(px, color);
				else
					igt_assert(px == 0);
			}
		}
	} else {
		buf = create_buf(&data, color);
		ptr = gem_mmap__device_coherent(drm_fd, buf->handle, 0,
						buf->surface[0].size, PROT_READ);
		igt_assert(wait_for_suspended());

		for (i = 0; i < WIDTH; i++)
			for (j = 0; j < HEIGHT; j++)
				buf_check(ptr, i, j, color);
		munmap(ptr, buf->surface[0].size);
	}

	/* Now resume and check for it again. */
	enable_one_screen_or_forcewake_get_and_wait(&ms_data);

	if (blt_has_xy_color(drm_fd)) {
		memset(cpu_buf, 0, dst_size);
		gem_read(drm_fd, handle, 0, cpu_buf, dst_size);

		for (y = 0; y < 128; y++) {
			for (x = 0; x < 128; x++) {
				uint32_t px = cpu_buf[y * 128 + x];

				if (y >= sq_y && y < (sq_y + sq_h) &&
				    x >= sq_x && x < (sq_x + sq_w))
					igt_assert_eq_u32(px, color);
				else
					igt_assert(px == 0);
			}
		}
	} else {
		buf = create_buf(&data, color);
		ptr = gem_mmap__device_coherent(drm_fd, buf->handle, 0,
						buf->surface[0].size, PROT_READ);
		for (i = 0; i < WIDTH; i++)
			for (j = 0; j < HEIGHT; j++)
				buf_check(ptr, i, j, color);
		munmap(ptr, buf->surface[0].size);
	}

	/* Now we'll do the opposite: do the blt while active, then read while
	 * suspended. We use the same spot, but a different color. As a bonus,
	 * we're testing the presumed_offset from the previous command. */
	color = 0x87654321;
	if (blt_has_xy_color(drm_fd)) {

		submit_blt_cmd(handle, dst_size, sq_x, sq_y, sq_w, sq_h, pitch, color,
			       &presumed_offset);

		disable_all_screens_or_forcewake_put_and_wait(&ms_data);

		memset(cpu_buf, 0, dst_size);
		gem_read(drm_fd, handle, 0, cpu_buf, dst_size);
		for (y = 0; y < 128; y++) {
			for (x = 0; x < 128; x++) {
				uint32_t px = cpu_buf[y * 128 + x];

				if (y >= sq_y && y < (sq_y + sq_h) &&
				    x >= sq_x && x < (sq_x + sq_w))
					igt_assert_eq_u32(px, color);
				else
					igt_assert(px == 0);
			}
		}
	} else {
		buf = create_buf(&data, color);
		ptr = gem_mmap__device_coherent(drm_fd, buf->handle, 0,
						buf->surface[0].size, PROT_READ);
		for (i = 0; i < WIDTH; i++)
			for (j = 0; j < HEIGHT; j++)
				buf_check(ptr, i, j, color);
		munmap(ptr, buf->surface[0].size);
	}

	gem_close(drm_fd, handle);

	free(cpu_buf);
}

/* Assuming execbuf already works, let's see what happens when we force many
 * suspend/resume cycles with commands. */
static void
gem_execbuf_stress_subtest(int rounds, int wait_flags,
			   struct drm_i915_gem_memory_class_instance *mem_regions)
{
	int i;
	int batch_size = 4 * sizeof(uint32_t);
	uint32_t batch_buf[batch_size];
	uint32_t handle;
	struct drm_i915_gem_execbuffer2 execbuf = {};
	struct drm_i915_gem_exec_object2 objs[1] = {{}};

	igt_require_gem(drm_fd);

	if (wait_flags & WAIT_PC8_RES)
		igt_require(has_pc8);

	i = 0;
	batch_buf[i++] = MI_NOOP;
	batch_buf[i++] = MI_NOOP;
	batch_buf[i++] = MI_BATCH_BUFFER_END;
	batch_buf[i++] = MI_NOOP;
	igt_assert(i * sizeof(uint32_t) == batch_size);

	disable_all_screens_and_wait(&ms_data);

	/* PC8 test is only applicable to igfx  */
	if (wait_flags & WAIT_PC8_RES)
		handle = gem_create(drm_fd, batch_size);
	else
		handle = gem_create_in_memory_region_list(drm_fd, batch_size, 0, mem_regions, 1);

	gem_write(drm_fd, handle, 0, batch_buf, batch_size);

	objs[0].handle = handle;

	execbuf.buffers_ptr = (uintptr_t)objs;
	execbuf.buffer_count = 1;
	execbuf.batch_len = batch_size;
	execbuf.flags = I915_EXEC_RENDER;
	i915_execbuffer2_set_context_id(execbuf, 0);

	for (i = 0; i < rounds; i++) {
		gem_execbuf(drm_fd, &execbuf);

		if (wait_flags & WAIT_STATUS) {
			/* clean up idle work */
			igt_drop_caches_set(drm_fd, DROP_IDLE);
			igt_assert(wait_for_suspended());
		}
		if (wait_flags & WAIT_PC8_RES)
			igt_assert(pc8_plus_residency_changed(30));
		if (wait_flags & WAIT_EXTRA)
			sleep(5);
	}

	gem_close(drm_fd, handle);
}

/* When this test was written, it triggered WARNs and DRM_ERRORs on dmesg. */
static void gem_idle_subtest(void)
{
	disable_all_screens_and_wait(&ms_data);

	sleep(5);

	gem_test_all_engines(drm_fd);
}

static void gem_evict_pwrite_subtest(void)
{
	struct {
		uint32_t handle;
		uint32_t *ptr;
	} *trash_bos;
	unsigned int num_trash_bos, n;
	uint32_t buf;

	num_trash_bos = gem_mappable_aperture_size(drm_fd) / (1024*1024) + 1;
	trash_bos = malloc(num_trash_bos * sizeof(*trash_bos));
	igt_assert(trash_bos);

	for (n = 0; n < num_trash_bos; n++) {
		trash_bos[n].handle = gem_create(drm_fd, 1024*1024);
		trash_bos[n].ptr = gem_mmap__gtt(drm_fd, trash_bos[n].handle,
						 1024*1024, PROT_WRITE);
		*trash_bos[n].ptr = 0;
	}

	disable_or_dpms_all_screens_and_wait(&ms_data, true);
	igt_assert(wait_for_suspended());

	buf = 0;
	for (n = 0; n < num_trash_bos; n++)
		gem_write(drm_fd, trash_bos[n].handle, 0, &buf, sizeof(buf));

	for (n = 0; n < num_trash_bos; n++) {
		munmap(trash_bos[n].ptr, 1024*1024);
		gem_close(drm_fd, trash_bos[n].handle);
	}
	free(trash_bos);
}

/* This also triggered WARNs on dmesg at some point. */
static void reg_read_ioctl_subtest(void)
{
	struct drm_i915_reg_read rr = {
		.offset = 0x2358, /* render ring timestamp */
	};

	disable_all_screens_and_wait(&ms_data);

	do_ioctl(drm_fd, DRM_IOCTL_I915_REG_READ, &rr);

	igt_assert(wait_for_suspended());
}

static bool device_in_pci_d3(struct pci_device *pci_dev)
{
	uint16_t val;
	int rc;

	rc = pci_device_cfg_read_u16(pci_dev, &val, 0xd4);
	igt_assert_eq(rc, 0);

	igt_debug("%s: PCI D3 state=%d\n", __func__, val & 0x3);
	return (val & 0x3) == 0x3;
}

__noreturn static void stay_subtest(void)
{
	disable_all_screens_and_wait(&ms_data);

	while (1)
		sleep(600);
}

static void system_suspend_subtest(int state, int debug)
{
	disable_all_screens_and_wait(&ms_data);

	igt_system_suspend_autoresume(state, debug);
	igt_assert(wait_for_suspended());
}

static void system_suspend_execbuf_subtest(void)
{
	int i;
	int batch_size = 4 * sizeof(uint32_t);
	uint32_t batch_buf[batch_size];
	uint32_t handle;
	struct drm_i915_gem_execbuffer2 execbuf = {};
	struct drm_i915_gem_exec_object2 objs[1] = {{}};

	i = 0;
	batch_buf[i++] = MI_NOOP;
	batch_buf[i++] = MI_NOOP;
	batch_buf[i++] = MI_BATCH_BUFFER_END;
	batch_buf[i++] = MI_NOOP;
	igt_assert(i * sizeof(uint32_t) == batch_size);

	handle = gem_create(drm_fd, batch_size);
	gem_write(drm_fd, handle, 0, batch_buf, batch_size);

	objs[0].handle = handle;

	execbuf.buffers_ptr = (uintptr_t)objs;
	execbuf.buffer_count = 1;
	execbuf.batch_len = batch_size;
	execbuf.flags = I915_EXEC_RENDER;
	i915_execbuffer2_set_context_id(execbuf, 0);

	disable_all_screens_and_wait(&ms_data);
	igt_system_suspend_autoresume(SUSPEND_STATE_MEM, SUSPEND_TEST_NONE);
	igt_assert(wait_for_suspended());

	for (i = 0; i < 20; i++) {
		gem_execbuf(drm_fd, &execbuf);
		igt_assert(wait_for_suspended());
	}

	gem_close(drm_fd, handle);
}

int rounds = 10;
bool stay = false;

static int opt_handler(int opt, int opt_index, void *data)
{
	switch (opt) {
	case 'l':
		rounds = 50;
		break;
	case 's':
		stay = true;
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

const char *help_str =
	"  --stress\t\tMake the stress-tests more stressful.\n"
	"  --stay\t\tDisable all screen and try to go into runtime pm. Useful for debugging.";
static struct option long_options[] = {
	{"stress", 0, 0, 'l'},
	{"stay", 0, 0, 's'},
	{ 0, 0, 0, 0 }
};

int igt_main_args("", long_options, help_str, opt_handler, NULL)
{
	/* Skip instead of failing in case the machine is not prepared to reach
	 * PC8+. We don't want bug reports from cases where the machine is just
	 * not properly configured. */
	igt_fixture() {
		igt_require(setup_environment(true));
	}

	if (stay)
		igt_subtest("stay")
			stay_subtest();

	/* GEM */
	igt_subtest_with_dynamic("gem-mmap-type") {
		for_each_mmap_offset_type(drm_fd, t) {
			for_each_memory_region(r, drm_fd) {
				igt_dynamic_f("%s-%s", t->name, r->name)
				gem_mmap_args(t, &r->ci);
			}
		}
	}

	igt_subtest("gem-pread")
		gem_pread_subtest();
	igt_subtest_with_dynamic("gem-execbuf") {
		for_each_memory_region(r, drm_fd) {
			igt_dynamic_f("%s", r->name)
				gem_execbuf_subtest(&r->ci);
		}
	}
	igt_subtest("gem-idle")
		gem_idle_subtest();
	igt_subtest("gem-evict-pwrite") {
		gem_require_mappable_ggtt(drm_fd);
		gem_evict_pwrite_subtest();
	}

	/* Misc */
	igt_subtest("reg-read-ioctl")
		reg_read_ioctl_subtest();
	igt_subtest("debugfs-read")
		debugfs_read_subtest();
	igt_subtest("debugfs-forcewake-user")
		debugfs_forcewake_user_subtest();
	igt_subtest("sysfs-read")
		sysfs_read_subtest();

	/* System suspend */
	igt_subtest("system-suspend-devices")
		system_suspend_subtest(SUSPEND_STATE_MEM, SUSPEND_TEST_DEVICES);
	igt_subtest("system-suspend")
		system_suspend_subtest(SUSPEND_STATE_MEM, SUSPEND_TEST_NONE);
	igt_subtest("system-suspend-execbuf")
		system_suspend_execbuf_subtest();
	igt_subtest("system-hibernate-devices")
		system_suspend_subtest(SUSPEND_STATE_DISK,
				       SUSPEND_TEST_DEVICES);
	igt_subtest("system-hibernate")
		system_suspend_subtest(SUSPEND_STATE_DISK, SUSPEND_TEST_NONE);

	/* GEM stress */
	igt_describe("Validate execbuf submission while exercising rpm "
		     "suspend/resume cycles.");
	igt_subtest_with_dynamic("gem-execbuf-stress") {
		for_each_memory_region(r, drm_fd) {
			igt_dynamic_f("%s", r->name)
				gem_execbuf_stress_subtest(rounds, WAIT_STATUS, &r->ci);
			igt_dynamic_f("%s-%s", "extra-wait", r->name)
				gem_execbuf_stress_subtest(rounds, WAIT_STATUS | WAIT_EXTRA, &r->ci);
		}
	}

	igt_subtest("gem-execbuf-stress-pc8")
		gem_execbuf_stress_subtest(rounds, WAIT_PC8_RES, 0);

	igt_fixture() {
		teardown_environment(true);
		forcewake_put(&ms_data);
	}

	igt_subtest("module-reload") {
		struct pci_device *pci_dev;

		igt_debug("Reload w/o display\n");
		igt_i915_driver_unload();

		igt_kmsg(KMSG_INFO "Reloading i915 w/o display\n");
		igt_assert_eq(igt_i915_driver_load("disable_display=1 mmio_debug=-1"), 0);

		igt_assert(setup_environment(false));
		pci_dev = igt_device_get_pci_device(drm_fd);
		igt_assert(igt_wait(device_in_pci_d3(pci_dev), 2000, 100));
		teardown_environment(false);

		igt_debug("Reload as normal\n");
		igt_i915_driver_unload();

		igt_kmsg(KMSG_INFO "Reloading i915 as normal\n");
		igt_assert_eq(igt_i915_driver_load("mmio_debug=-1"), 0);

		igt_assert(setup_environment(true));
		pci_dev = igt_device_get_pci_device(drm_fd);
		igt_assert(igt_wait(device_in_pci_d3(pci_dev), 2000, 100));
		teardown_environment(true);

		/* Remove our mmio_debugging module */
		igt_i915_driver_unload();
	}
}
