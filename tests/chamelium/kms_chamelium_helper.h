/*
 * Copyright © 2016 Red Hat Inc.
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
 *    Lyude Paul <lyude@redhat.com>
 */

#ifndef TESTS_CHAMELIUM_CHAMELIUM_HELPER_H
#define TESTS_CHAMELIUM_CHAMELIUM_HELPER_H

#include "igt.h"

#define ONLINE_TIMEOUT 20 /* seconds */

#define for_each_port(p, port)                                 \
	for (p = 0, port = data.ports[p]; p < data.port_count; \
	     p++, port = data.ports[p])

/**
 * connector_subtest() - Run a subtest for all ports of a given connector type
 *
 * Defines a subtest that iterates over all available Chamelium ports and
 * executes the provided test function for each port matching the requested
 * connector type. If no matching port is found, the subtest is skipped
 * cleanly.
 *
 * This helper avoids repeated connector-iteration logic and provides a
 * consistent pattern for running connector-specific tests.
 */

#define connector_subtest(name__, connector_type__, data__, test_fn__, ...)		\
	do {										\
		igt_subtest(name__) {							\
			int p__;							\
			struct chamelium_port *port__;					\
			bool found__ = false;						\
			for_each_port(p__, port__) {					\
				if (chamelium_port_get_type(port__) !=			\
					DRM_MODE_CONNECTOR_##connector_type__)		\
					continue;					\
				igt_info("%s: testing port %s\n", name__,		\
					 chamelium_port_get_name(port__));		\
				found__ = true;						\
				test_fn__(data__, port__, ##__VA_ARGS__);		\
			}								\
			if (!found__)							\
				igt_skip(#connector_type__				\
					" connector not available");			\
			}								\
	} while (0)									\

/*
 * The chamelium data structure is used to store all the information known about
 * chamelium to run the tests.
 */
typedef struct {
	struct chamelium *chamelium;
	struct chamelium_port **ports;
	igt_display_t display;
	int port_count;

	int drm_fd;

	struct chamelium_edid *edids[IGT_CUSTOM_EDID_COUNT];
} chamelium_data_t;

void chamelium_init_test(chamelium_data_t *data);

bool chamelium_wait_for_hotplug(struct udev_monitor *mon, int *timeout);
void chamelium_wait_for_connector_after_hotplug(chamelium_data_t *data,
						struct udev_monitor *mon,
						struct chamelium_port *port,
						drmModeConnection status);

igt_output_t *chamelium_prepare_output(chamelium_data_t *data,
				       struct chamelium_port *port,
				       enum igt_custom_edid_type edid);
void chamelium_enable_output(chamelium_data_t *data,
			     struct chamelium_port *port, igt_output_t *output,
			     drmModeModeInfo *mode, struct igt_fb *fb);
igt_crtc_t * chamelium_get_pipe_for_output(igt_display_t *display,
					igt_output_t *output);

int chamelium_get_pattern_fb(chamelium_data_t *data, size_t width,
			     size_t height, uint32_t fourcc, size_t block_size,
			     struct igt_fb *fb);
void chamelium_create_fb_for_mode(chamelium_data_t *data, struct igt_fb *fb,
				  drmModeModeInfo *mode);
drmModeModeInfo chamelium_get_mode_for_port(struct chamelium *chamelium,
					    struct chamelium_port *port);
igt_output_t *chamelium_get_output_for_port(chamelium_data_t *data,
					    struct chamelium_port *port);

void chamelium_set_edid(chamelium_data_t *data, struct chamelium_port *port,
			enum igt_custom_edid_type edid);

bool chamelium_check_analog_bridge(chamelium_data_t *data,
				   struct chamelium_port *port);

#endif /* TESTS_CHAMELIUM_CHAMELIUM_HELPER_H */
