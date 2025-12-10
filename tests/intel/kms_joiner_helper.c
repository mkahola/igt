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

static enum pipe get_next_master_pipe(uint32_t pipe_mask)
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
	enum pipe pipe;

	*master_pipes = 0;
	for (pipe = PIPE_A; pipe < IGT_MAX_PIPES - 1; pipe++) {
		if (igt_crtc_for_pipe(display, pipe)->valid && igt_crtc_for_pipe(display, pipe + 1)->valid) {
			*master_pipes |= BIT(pipe);
			igt_info("Found master pipe %s\n", kmstest_pipe_name(pipe));
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
	enum pipe mp = PIPE_NONE;
	igt_output_t *out;

	for (idx = 0; idx < num_outputs; idx++) {
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

		if (needed > 1) {
			mp = get_next_master_pipe(BIT(start));

			if (mp == PIPE_NONE) {
				igt_debug("Failed to confirm master pipe for %s\n",
						out->name);
				return false;
			}
			igt_output_set_crtc(out,
					    igt_crtc_for_pipe(out->display, start));
			igt_debug("Using pipe %s as master.\n",
					kmstest_pipe_name(start));
		} else
			igt_output_set_crtc(out,
					    igt_crtc_for_pipe(out->display, start));

		for (i = 0; i < needed; i++)
			*used_pipes_mask |= BIT(start + i);
	}
	return true;
}
