// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include <inttypes.h>

#include "igt_vrr.h"
#include "igt_sysfs.h"

/**
 * Integer refresh rates that sinks advertise as standard video timings, and
 * for which a fractional (rate * 1000/1001) counterpart is also defined:
 *
 *  - 24, 25, 30, 50 and 60 Hz are the film and broadcast (PAL/NTSC) cadences
 *    carried over into the CTA-861 video formats.
 *  - 48, 100, 120, 200 and 240 Hz are the integer multiples of those
 *    cadences, also listed as CTA-861 video formats.
 *  - 75, 90, 96, 144, 165 and 180 Hz are VESA DMT/CVT and adaptive-sync panel
 *    rates in common use.
 *
 * The fractional variant of each entry (e.g. 60 -> 59.94) is what a target
 * refresh rate in video mode programs.
 */
const uint32_t igt_vrr_standard_video_timing_fps[] = {
	24, 25, 30, 48, 50, 60, 75, 90, 96, 100, 120, 144, 165, 180, 200, 240,
};

const uint32_t igt_vrr_standard_video_timing_fps_count =
	ARRAY_SIZE(igt_vrr_standard_video_timing_fps);

/**
 * igt_vrr_target_rr_debugfs_write:
 * @fd: DRM file descriptor.
 * @crtc_index: Index of the CRTC.
 * @rr_numerator: Numerator of the target refresh rate fraction.
 * @rr_denominator: Denominator of the target refresh rate fraction.
 *
 * Write the target refresh rate configuration to the per-CRTC
 * VRR debugfs interface. Passing 0/0 clears the target refresh
 * rate.
 *
 * The debugfs interface is Intel specific, so this returns false on
 * other drivers. Other drivers can add their own debugfs node here.
 *
 * Returns:
 * true if the target refresh rate was written successfully, false otherwise.
 */
bool
igt_vrr_target_rr_debugfs_write(int fd, int crtc_index,
				uint32_t rr_numerator,
				uint32_t rr_denominator)
{
	char buf[32];
	int ret, dir, len;

	if (!is_intel_device(fd)) {
		igt_info("Not an Intel device\n");
		return false;
	}

	len = snprintf(buf, sizeof(buf), "%u/%u", rr_numerator, rr_denominator);
	if (len <= 0 || len >= (int)sizeof(buf))
		return false;

	dir = igt_debugfs_crtc_dir(fd, crtc_index);
	if (dir < 0)
		return false;

	ret = igt_sysfs_write(dir, "intel_vrr_target_refresh_rate", buf, len);
	close(dir);

	return ret == len;
}

/**
 * igt_vrr_target_rr_debugfs_read:
 * @fd: DRM file descriptor.
 * @crtc_index: Index of the CRTC.
 * @buf: Buffer to store the target refresh rate string.
 * @size: Size of @buf in bytes.
 *
 * Read the target refresh rate from the per-CRTC VRR debugfs interface.
 * The returned string is always NUL-terminated on success.
 *
 * The debugfs interface is Intel specific, so this returns false on
 * other drivers. Other drivers can add their own debugfs node here.
 *
 * Return:
 * true if the target refresh rate was read successfully, false otherwise.
 */
bool
igt_vrr_target_rr_debugfs_read(int fd, int crtc_index, char *buf, size_t size)
{
	int ret, dir;

	if (!buf || !size)
		return false;

	if (!is_intel_device(fd)) {
		igt_info("Not an Intel device\n");
		return false;
	}

	dir = igt_debugfs_crtc_dir(fd, crtc_index);
	if (dir < 0)
		return false;

	ret = igt_sysfs_read(dir, "intel_vrr_target_refresh_rate",
			     buf, size - 1);
	close(dir);

	if (ret < 0)
		return false;

	buf[ret] = '\0';

	return true;
}

/**
 * igt_vrr_mode_line_refresh_hz:
 * @mode: DRM display mode used for the calculation
 *
 * Compute the refresh rate directly from the mode timing parameters.
 *
 * Returns: Refresh rate in Hz as a floating-point value.
 */
double igt_vrr_mode_line_refresh_hz(const drmModeModeInfo *mode)
{
	return (double)mode->clock * 1000.0 / ((double)mode->htotal * (double)mode->vtotal);
}

/**
 * igt_vrr_target_refresh_rate_supported:
 * @fd: DRM device file descriptor.
 * @crtc_index: Index of the CRTC.
 *
 * Checks whether the target refresh rate debugfs node is present for the
 * specified CRTC, indicating CMRR support.
 *
 * The debugfs interface is Intel specific, so this returns false on
 * other drivers. Other drivers can add their own debugfs node here.
 *
 * Returns: true if CMRR is supported, false otherwise.
 */
bool igt_vrr_target_refresh_rate_supported(int fd, int crtc_index)
{
	int dir;

	if (!is_intel_device(fd))
		return false;

	dir = igt_debugfs_crtc_dir(fd, crtc_index);

	if (dir < 0)
		return false;

	if (faccessat(dir, "intel_vrr_target_refresh_rate", F_OK, 0) == 0) {
		close(dir);
		return true;
	}

	close(dir);
	return false;
}
