/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#include <fcntl.h>

#include "igt.h"
#include "igt_psr.h"

#include "intel_fbc.h"
#include "intel_wa.h"

#define FBC_STATUS_BUF_LEN 128

/**
 * intel_fbc_supported_on_chipset:
 * @device: fd of the device
 * @pipe: Display pipe
 *
 * Check if FBC is supported by chipset on given pipe.
 *
 * Returns:
 * true if FBC is supported and false otherwise.
 */
bool intel_fbc_supported_on_chipset(int device, enum pipe pipe)
{
	char buf[FBC_STATUS_BUF_LEN];
	int dir;

	dir = igt_debugfs_crtc_dir(device, pipe, O_DIRECTORY);
	igt_require_fd(dir);
	igt_debugfs_simple_read(dir, "i915_fbc_status", buf, sizeof(buf));
	close(dir);
	if (*buf == '\0')
		return false;

	return !strstr(buf, "FBC unsupported on this chipset\n") &&
		!strstr(buf, "stolen memory not initialised\n");
}

static bool _intel_fbc_is_enabled(int device, enum pipe pipe, int log_level, char *last_fbc_buf)
{
	char buf[FBC_STATUS_BUF_LEN];
	bool print = true;
	int dir;

	dir = igt_debugfs_crtc_dir(device, pipe, O_DIRECTORY);
	igt_require_fd(dir);
	igt_debugfs_simple_read(dir, "i915_fbc_status", buf, sizeof(buf));
	close(dir);
	if (log_level != IGT_LOG_DEBUG)
		last_fbc_buf[0] = '\0';
	else if (strcmp(last_fbc_buf, buf))
		strcpy(last_fbc_buf, buf);
	else
		print = false;

	if (print)
		igt_log(IGT_LOG_DOMAIN, log_level, "fbc_is_enabled():\n%s\n", buf);

	return strstr(buf, "FBC enabled\n");
}

/**
 * intel_fbc_is_enabled:
 * @device: fd of the device
 * @pipe: Display pipe
 * @log_level: Wanted loglevel
 *
 * Check if FBC is enabled on given pipe. Loglevel can be used to
 * control at which loglevel current state is printed out.
 *
 * Returns:
 * true if FBC is enabled.
 */
bool intel_fbc_is_enabled(int device, enum pipe pipe, int log_level)
{
	char last_fbc_buf[FBC_STATUS_BUF_LEN] = {'\0'};

	return _intel_fbc_is_enabled(device, pipe, log_level, last_fbc_buf);
}

/**
 * intel_fbc_wait_until_enabled:
 * @device: fd of the device
 * @pipe: Display pipe
 *
 * Wait until fbc is enabled. Used timeout is constant 2 seconds.
 *
 * Returns:
 * true if FBC got enabled.
 */
bool intel_fbc_wait_until_enabled(int device, enum pipe pipe)
{
	char last_fbc_buf[FBC_STATUS_BUF_LEN] = {'\0'};
	bool enabled = igt_wait(_intel_fbc_is_enabled(device, pipe, IGT_LOG_DEBUG, last_fbc_buf), 2000, 1);

	if (!enabled)
		igt_info("FBC is not enabled: \n%s\n", last_fbc_buf);

	return enabled;
}

/**
 * intel_fbc_max_plane_size
 *
 * @fd: fd of the device
 * @width: To get the max supported width
 * @height: To get the max supported height
 *
 * Function to update maximum plane size supported by FBC per platform
 *
 * Returns:
 * None
 */
void intel_fbc_max_plane_size(int fd, uint32_t *width, uint32_t *height)
{
	const uint32_t dev_id = intel_get_drm_devid(fd);
	const struct intel_device_info *info = intel_get_device_info(dev_id);
	int ver = info->graphics_ver;

	if (ver >= 10) {
		*width = 5120;
		*height = 4096;
	} else if (ver >= 8 || IS_HASWELL(fd)) {
		*width = 4096;
		*height = 4096;
	} else if (IS_G4X(fd) || ver >= 5) {
		*width = 4096;
		*height = 2048;
	} else {
		*width = 2048;
		*height = 1536;
	}
}


/**
 * intel_fbc_plane_size_supported
 *
 * @fd: fd of the device
 * @width: width of the plane to be checked
 * @height: height of the plane to be checked
 *
 * Checks if the plane size is supported for FBC
 *
 * Returns:
 * true if plane size is within the range as per the FBC supported size restrictions per platform
 */
bool intel_fbc_plane_size_supported(int fd, uint32_t width, uint32_t height)
{
	unsigned int max_w, max_h;

	intel_fbc_max_plane_size(fd, &max_w, &max_h);

	return width <= max_w && height <= max_h;
}

/**
 * intel_fbc_supported_for_psr_mode
 *
 * @disp_ver: Display version
 * @mode: psr mode
 *
 * FBC and PSR1/PSR2/PR combination support depends on the display version.
 *
 * Returns:
 * true if FBC and the given PSR mode can be enabled together in a platform
 */
bool intel_fbc_supported_for_psr_mode(int disp_ver, enum psr_mode mode)
{
	bool fbc_supported = true;

	switch(mode) {
	case PSR_MODE_1:
		/* TODO: Update this to exclude MTL C0 onwards from this list */
		if (disp_ver >= 12 && disp_ver <= 14)
			fbc_supported = false;
		break;
	case PSR_MODE_2:
	case PSR_MODE_2_SEL_FETCH:
	case PSR_MODE_2_ET:
	case PR_MODE_SEL_FETCH:
	case PR_MODE_SEL_FETCH_ET:
		/*
		 * FBC is not supported if PSR2 is enabled in display version 12 to 14.
		 * According to the xe2lpd+ requirements, display driver need to
		 * implement a selection logic between FBC and PSR2/Panel Replay selective
		 * update based on dirty region threshold. Until that is implemented,
		 * keep FBC disabled if PSR2/PR selective update is on.
		 *
		 * TODO: Update this based on the selection logic in the driver
		 */
		fbc_supported = false;
		break;
	case PR_MODE:
	default:
		break;
	}

	return fbc_supported;
}

/**
 * intel_is_fbc_disabled_by_wa
 *
 * @fd: fd of the device
 *
 * This function check if WA is present on some GT, which in turn make
 * FBC not possible
 *
 * Returns:
 * true: if WA is applied and FBC id disabled
 * false: otherwise
 */
bool intel_is_fbc_disabled_by_wa(int fd)
{
	int wa;
	const char *wa_fbc_disabled = "16023588340";

	if (!is_xe_device(fd))
		return false;

	wa = igt_has_intel_wa(fd, wa_fbc_disabled);
	igt_assert_f(wa >= 0, "WA path not found on GTs\n");

	return wa == 1;
}
