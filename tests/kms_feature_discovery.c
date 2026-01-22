/*
 * Copyright © 2020 Intel Corporation
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
 */

/**
 * TEST: kms feature discovery
 * Category: Display
 * Description: A metatest that checks for \"features\" presence.
 *		The subtests here should only skip or pass,
 *		anything else means we have a serious problem.
 * Driver requirement: i915, xe
 * Mega feature: General Display Features
 */

#include "igt.h"
#ifdef HAVE_CHAMELIUM
#include "igt_chamelium.h"
#endif
#include "igt_kms.h"
#include "igt_psr.h"
#include "igt_sysfs.h"
#include "igt_types.h"

/**
 * SUBTEST: display
 * Description: Make sure that we have display support.
 *
 * SUBTEST: display-%dx
 * Description: Make sure that we have display support with %arg[1]
 * 		outputs connected.
 *
 * SUBTEST: chamelium
 * Description: Make sure that Chamelium is configured and reachable.
 *
 * SUBTEST: psr1
 * Description: Make sure that we have eDP panel with PSR1 support.
 * Mega feature: PSR
 *
 * SUBTEST: psr2
 * Description: Make sure that we have eDP panel with PSR2 support.
 * Mega feature: PSR
 *
 * SUBTEST: dp-mst
 * Description: Make sure that we have DP-MST configuration.
 *
 * arg[1].values: 1, 2, 3, 4
 */

static igt_display_t display;

IGT_TEST_DESCRIPTION("A metatest that checks for \"features\" presence. "
		     "The subtests here should only skip or pass, "
		     "anything else means we have a serious problem.");
int igt_main() {
	igt_fd_t(debugfs_fd);
	igt_fd_t(fd);

	igt_fixture() {
		fd = drm_open_driver_master(DRIVER_ANY);
		debugfs_fd = igt_debugfs_dir(fd);
	}

	igt_subtest_group() {
		igt_fixture() {
			igt_display_require(&display, fd);
		}

		igt_describe("Make sure that we have display support.");
		igt_subtest("display") {
			/* will skip because of the fixture */
		}

		igt_subtest_group() {
			volatile int output_count = 0;
			igt_output_t *output;
			enum pipe pipe;

			igt_fixture() {
				/* this is what most of the 2x tests are doing */
				for_each_pipe(&display, pipe) {
					for_each_valid_output_on_pipe(&display, pipe, output) {
						if (igt_output_get_driving_pipe(output) == NULL) {
							igt_output_set_crtc(output,
									    igt_crtc_for_pipe(output->display, pipe));
							output_count++;
							break;
						}
					}
				}

				for (int i = 0; i < display.n_outputs; i++) {
					igt_output_set_crtc(&display.outputs[i],
							    NULL);
				}
			}

			igt_describe("Make sure that we can use at least 1 output at the same time.");
			igt_subtest("display-1x") {
				igt_require(output_count >= 1);
			}

			igt_describe("Make sure that we can use at least 2 outputs at the same time.");
			igt_subtest("display-2x") {
				igt_require(output_count >= 2);
			}

			igt_describe("Make sure that we can use at least 3 outputs at the same time.");
			igt_subtest("display-3x") {
				igt_require(output_count >= 3);
			}

			igt_describe("Make sure that we can use at least 4 outputs at the same time.");
			igt_subtest("display-4x") {
				igt_require(output_count >= 4);
			}
		}

#ifdef HAVE_CHAMELIUM
		igt_describe("Make sure that Chamelium is configured and reachable.");
		igt_subtest("chamelium") {
			struct chamelium *chamelium =
				chamelium_init(fd, &display);
			igt_require(chamelium);
			chamelium_deinit(chamelium);
		}
#endif

		igt_describe("Make sure that we have eDP panel with PSR1 support.");
		igt_subtest("psr1") {
			igt_require(psr_sink_support(fd, debugfs_fd, PSR_MODE_1, NULL));
		}

		igt_describe("Make sure that we have eDP panel with PSR2 support.");
		igt_subtest("psr2") {
			igt_require(psr_sink_support(fd, debugfs_fd, PSR_MODE_2, NULL));
		}

		igt_describe("Make sure that we have DP-MST configuration.");
		igt_subtest("dp-mst") {
			struct kmstest_connector_config config;
			igt_output_t *output;
			const char *encoder;
			int ret = -1;

			for_each_connected_output(&display, output) {
				kmstest_get_connector_config(fd, output->config.connector->connector_id, -1, &config);
				encoder = kmstest_encoder_type_str(config.encoder->encoder_type);

				ret = strcmp(encoder, "DP MST");
				if (ret == 0)
					break;
			}
			igt_require_f(ret == 0, "No DP-MST configuration found.\n");
		}
	}
}
