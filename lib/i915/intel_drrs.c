/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#include <fcntl.h>

#include "igt.h"
#include "igt_sysfs.h"

#include "intel_drrs.h"

/**
 * intel_is_drrs_supported:
 * @crtc: CRTC
 *
 * Check if DRRS is supported on given CRTC.
 *
 * Returns:
 * true if DRRS is supported and false otherwise.
 */
bool intel_is_drrs_supported(igt_crtc_t *crtc)
{
	char buf[256];
	int dir;

	dir = igt_crtc_debugfs_dir(crtc, O_DIRECTORY);
	igt_require_fd(dir);
	igt_debugfs_simple_read(dir, "i915_drrs_status", buf, sizeof(buf));
	close(dir);

	return strstr(buf, "DRRS capable: yes");
}

/**
 * intel_output_has_drrs
 * @device: fd of the device
 * @output: Display output
 *
 * Check if drrs used on given output.
 *
 * Returns:
 * true if DRRS is used and false otherwise.
 */
bool intel_output_has_drrs(int device, igt_output_t *output)
{
	char buf[256];
	int dir;

	dir = igt_debugfs_connector_dir(device, output->name, O_DIRECTORY);
	igt_require_fd(dir);
	igt_debugfs_simple_read(dir, "i915_drrs_type", buf, sizeof(buf));
	close(dir);

	return strstr(buf, "seamless");
}

static void drrs_set(int device, int crtc_index, unsigned int val)
{
	char buf[2];
	int dir, ret;

	igt_debug("Manually %sabling DRRS. %u\n", val ? "en" : "dis", val);
	snprintf(buf, sizeof(buf), "%d", val);

	dir = igt_debugfs_crtc_dir(device, crtc_index, O_DIRECTORY);
	igt_require_fd(dir);
	ret = igt_sysfs_write(dir, "i915_drrs_ctl", buf, sizeof(buf) - 1);
	close(dir);

	/*
	 * drrs_enable() is called on DRRS capable platform only,
	 * whereas drrs_disable() is called on all platforms.
	 * So handle the failure of debugfs_write only for drrs_enable().
	 */
	if (val)
		igt_assert_f(ret == (sizeof(buf) - 1), "debugfs_write failed");
}

/**
 * intel_drrs_enable:
 * @crtc: CRTC
 *
 * Enable DRRS on given CRTC.
 *
 * Returns:
 * none
 */
void intel_drrs_enable(igt_crtc_t *crtc)
{
	drrs_set(crtc->display->drm_fd, crtc->crtc_index, 1);
}

/**
 * intel_drrs_disable:
 * @crtc: CRTC
 *
 * Disable DRRS on given CRTC.
 *
 * Returns:
 * none
 */
void intel_drrs_disable(igt_crtc_t *crtc)
{
	drrs_set(crtc->display->drm_fd, crtc->crtc_index, 0);
}

/* FIXME: Remove this and its users. */
void intel_drrs_disable_crtc_index(int device, int crtc_index)
{
	drrs_set(device, crtc_index, 0);
}

/**
 * intel_is_drrs_inactive:
 * @crtc: CRTC
 *
 * Check if drrs is inactive on given CRTC.
 *
 * Returns:
 * true if inactive and false otherwise
 */
bool intel_is_drrs_inactive(igt_crtc_t *crtc)
{
	char buf[256];
	int dir;

	dir = igt_crtc_debugfs_dir(crtc, O_DIRECTORY);
	igt_require_fd(dir);
	igt_debugfs_simple_read(dir, "i915_drrs_status", buf, sizeof(buf));
	close(dir);

	return strstr(buf, "DRRS active: no");
}
