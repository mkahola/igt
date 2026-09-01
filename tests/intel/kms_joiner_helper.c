/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#include "kms_joiner_helper.h"
#include "igt.h"
#include "igt_kms.h"
#include "intel_chipset.h"

/*
 * Detect if the output needs 1, 2, or 4 pipes (non-joiner, big joiner, ultra).
 */
static int get_required_pipes(int drm_fd, igt_output_t *output)
{
	bool is_big = false, is_ultra = false;
	int max_dotclock;
	drmModeModeInfo mode;

	if (!is_intel_device(drm_fd))
		return -1;

	max_dotclock = igt_get_max_dotclock(drm_fd);

	is_ultra = ultrajoiner_mode_found(drm_fd,
			output->config.connector,
			max_dotclock,
			&mode);
	is_big = bigjoiner_mode_found(drm_fd,
			output->config.connector,
			max_dotclock,
			&mode);

	if (is_ultra)
		return 4;
	if (is_big)
		return 2;

	return 1;
}

/*
 * Internal helper to find a block of consecutive free pipes
 * in available_pipes_mask. If count > 1, the first pipe must also
 * be in master_pipes_mask.
 *
 * Returns the starting pipe index or -1 if not found.
 */
static int find_consecutive_pipes(int n_pipes,
				  uint32_t available_pipes_mask,
				  uint32_t master_pipes_mask,
				  int count)
{
	int i = 0, pipe_idx = 0;
	bool can_use;

	for (int start = 0; start < n_pipes; start++) {
		if (((start + count) - 1) >= n_pipes)
			break;

		if ((count > 1) && (!(BIT(start) & master_pipes_mask)))
			continue;

		can_use = true;
		for (i = 0; i < count; i++) {
			pipe_idx = start + i;
			if (!(BIT(pipe_idx) & available_pipes_mask)) {
				can_use = false;
				break;
			}
		}
		if (can_use)
			return start;
	}
	return -1;
}

static enum hardware_pipe get_next_master_pipe(uint32_t pipe_mask)
{
	int i;

	if (!pipe_mask)
		return PIPE_NONE;

	i = ffs(pipe_mask) - 1;

	if (i < 0)
		return PIPE_NONE;

	return i;
}

/**
 * igt_set_all_master_pipes_for_platform:
 * @master_pipes: Pointer to the variable to store the master pipes bitmask.
 * @display: The display structure containing pipe information.
 *
 * This function sets the master pipes for the platform by checking if consecutive
 * pipes are enabled. If both pipe and the next pipe are enabled, the pipe is
 * considered a master pipe.
 */
void igt_set_all_master_pipes_for_platform(igt_display_t *display, uint32_t *master_pipes)
{
	igt_crtc_t *crtc;

	*master_pipes = 0;

	for_each_crtc(display, crtc) {
		if (igt_crtc_for_pipe(display, crtc->hardware_pipe + 1)) {
			*master_pipes |= BIT(crtc->hardware_pipe);
			igt_info("Found master pipe %s\n", igt_crtc_name(crtc));
		}
	}
}

/*
 * @drm_fd: DRM file descriptor
 * @outputs: array of pointers to igt_output_t
 * @num_outputs: how many outputs in the array
 * @n_pipes: total number of pipes available
 * @used_pipes_mask: pointer to a bitmask (in/out) of already-used pipes
 * @master_pipes_mask: bitmask of valid "master" pipes
 * @valid_pipes_mask: bitmask of valid (non-fused) pipes
 *
 * Assign pipes to outputs based on the number of required pipes.
 * This function will assign 1, 2, or 4 consecutive pipes to each output.
 * It will also mark the used pipes in the bitmask.
 *
 * Returns: true if all outputs can be assigned successfully; false otherwise.
 */
bool igt_assign_pipes_for_outputs(int drm_fd,
				  igt_output_t **outputs,
				  int num_outputs,
				  int n_pipes,
				  uint32_t *used_pipes_mask,
				  uint32_t master_pipes_mask,
				  uint32_t valid_pipes_mask)
{
	int i = 0, idx  = 0, needed = 0, start = 0;
	uint32_t available_pipes_mask = 0;
	enum hardware_pipe mp = PIPE_NONE;
	igt_output_t *out;

	for (idx = 0; idx < num_outputs; idx++) {
		igt_crtc_t *crtc;

		out = outputs[idx];
		needed = get_required_pipes(drm_fd, out);
		if (needed < 0)
			return false;

		available_pipes_mask = (~(*used_pipes_mask)) & valid_pipes_mask;
		start = find_consecutive_pipes(n_pipes, available_pipes_mask,
				master_pipes_mask, needed);

		if (start < 0) {
			igt_debug("Cannot allocate %d consecutive pipes for output %s\n",
					needed, out->name);
			return false;
		}

		igt_info("Assigning %d pipes [start=%s..%s] to output %s\n",
				needed, kmstest_pipe_name(start),
				kmstest_pipe_name(start + needed - 1), out->name);

		crtc = igt_crtc_for_pipe(out->display, start);
		igt_assert_f(crtc, "There is no pipe %s\n", kmstest_pipe_name(start));

		if (needed > 1) {
			mp = get_next_master_pipe(BIT(start));

			if (mp == PIPE_NONE) {
				igt_debug("Failed to confirm master pipe for %s\n",
						out->name);
				return false;
			}
			igt_debug("Using pipe %s as master.\n", igt_crtc_name(crtc));
		}

		igt_output_set_crtc(out, crtc);

		for (i = 0; i < needed; i++)
			*used_pipes_mask |= BIT(start + i);
	}
	return true;
}

/**
 * intel_max_hdisplay_non_joiner_mode_found:
 * @drm_fd: drm file descriptor
 * @connector: libdrm connector
 * @max_dot_clock: max dot clock frequency
 * @mode: libdrm mode to be filled
 *
 * Find the mode with maximum hdisplay that does not require bigjoiner.
 *
 * Bigjoiner is triggered when EITHER:
 * - hdisplay > max_pipe_hdisplay (platform's single-pipe width limit)
 * - clock > max_dotclock (platform's clock limit)
 *
 * Prioritizes hdisplay as the primary metric, using clock as a
 * tiebreaker when multiple modes have identical hdisplay. This is correct for
 * display testing where maximum supported resolution is the primary concern.
 *
 * Returns: true if a valid non-joiner mode exists, false otherwise.
 */
bool intel_max_hdisplay_non_joiner_mode_found(int drm_fd, drmModeConnector *connector,
					     int max_dotclock, drmModeModeInfo *mode)
{
	bool found = false;

	for (int i = 0; i < connector->count_modes; i++) {
		drmModeModeInfo *current_mode = &connector->modes[i];

	if (igt_bigjoiner_possible(drm_fd, current_mode, max_dotclock))
		continue;

	if (!found || current_mode->hdisplay > mode->hdisplay ||
	    (current_mode->hdisplay == mode->hdisplay &&
	     current_mode->clock > mode->clock)) {
		*mode = *current_mode;
		found = true;
	}
	}

	return found;
}

bool intel_mode_needs_joiner(int drm_fd, drmModeModeInfo *mode)
{
	int max_dotclock = igt_get_max_dotclock(drm_fd);

	return igt_bigjoiner_possible(drm_fd, mode, max_dotclock) ||
	       igt_ultrajoiner_possible(drm_fd, mode, max_dotclock);
}

/**
 * igt_is_joiner_supported_by_source:
 * @drm_fd: drm file descriptor
 * @joiner_type: joiner type
 *
 * Returns: True if (ultra|big)joiner is supported by platform, false otherwise
 */
bool igt_is_joiner_supported_by_source(int drm_fd, enum joined_pipes joiner_type)
{
	int disp_ver;
	bool is_dgfx;

	is_dgfx = is_xe_device(drm_fd) ? xe_has_vram(drm_fd) : gem_has_lmem(drm_fd);
	disp_ver = intel_display_ver(intel_get_drm_devid(drm_fd));

	switch (joiner_type) {
	case JOINED_PIPES_BIG_JOINER:
		if (disp_ver < 12) {
			igt_info("Bigjoiner is not supported on D11 and older platforms.\n");
			return false;
		}
		return true;
	case JOINED_PIPES_ULTRA_JOINER:
		if (!is_dgfx || disp_ver != 14) {
			igt_info("Ultrajoiner is supported on dgfx with D14 only.\n");
			return false;
		}
		return true;
	default:
		return false;
	}
}

/**
 * igt_get_joined_pipes_name:
 * @val: joined_pipes enum value
 *
 * Returns: A readable string for the given joined_pipes enum value.
 */
const char *igt_get_joined_pipes_name(enum joined_pipes val)
{
	switch (val) {
	case JOINED_PIPES_DEFAULT:
		return "";
	case JOINED_PIPES_NONE:
		return "none";
	case JOINED_PIPES_BIG_JOINER:
		return "bigjoiner";
	case JOINED_PIPES_ULTRA_JOINER:
		return "ultrajoiner";
	default:
		igt_assert(false);
	}
}
