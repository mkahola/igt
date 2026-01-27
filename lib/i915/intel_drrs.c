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
 * @device: fd of the device
 * @pipe: Display pipe
 *
 * Check if DRRS is supported on given pipe.
 *
 * Returns:
 * true if DRRS is supported and false otherwise.
 */
bool intel_is_drrs_supported(int device, enum pipe pipe)
{
	char buf[256];
	int dir;

	dir = igt_debugfs_crtc_dir(device, pipe, O_DIRECTORY);
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

static void drrs_set(int device, enum pipe pipe, unsigned int val)
{
	char buf[2];
	int dir, ret;

	igt_debug("Manually %sabling DRRS. %u\n", val ? "en" : "dis", val);
	snprintf(buf, sizeof(buf), "%d", val);

	dir = igt_debugfs_crtc_dir(device, pipe, O_DIRECTORY);
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
 * @device: fd of the device
 * @pipe: Display pipe
 *
 * Enable DRRS on given pipe
 *
 * Returns:
 * none
 */
void intel_drrs_enable(int device, enum pipe pipe)
{
	drrs_set(device, pipe, 1);
}

/**
 * intel_drrs_disable:
 * @device: fd of the device
 * @pipe: Display pipe
 *
 * Disable DRRS on given pipe
 *
 * Returns:
 * none
 */
void intel_drrs_disable(int device, enum pipe pipe)
{
	drrs_set(device, pipe, 0);
}

/**
 * intel_is_drrs_inactive:
 * @device: fd of the device
 * @pipe: Display pipe
 *
 * Check if drrs is inactive on given pipe
 *
 * Returns:
 * true if inactive and false otherwise
 */
bool intel_is_drrs_inactive(int device, enum pipe pipe)
{
	char buf[256];
	int dir;

	dir = igt_debugfs_crtc_dir(device, pipe, O_DIRECTORY);
	igt_require_fd(dir);
	igt_debugfs_simple_read(dir, "i915_drrs_status", buf, sizeof(buf));
	close(dir);

	return strstr(buf, "DRRS active: no");
}
