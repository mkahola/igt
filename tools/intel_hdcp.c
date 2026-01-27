// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include <fcntl.h>
#include <stdio.h>

#include "igt.h"

#define MAX_HDCP_BUF_LEN	5000

#define CP_UNDESIRED	0
#define CP_DESIRED	1
#define CP_ENABLED	2

enum hdcp_type {
	HDCP_TYPE_NONE,
	HDCP_TYPE_1_4,
	HDCP_TYPE_2_2_TYPE_0,
	HDCP_TYPE_2_2_TYPE_1
};

typedef struct data {
	int fd;
	igt_display_t display;
	struct igt_fb fb;
	int height, width;
	igt_output_t *output;
	enum pipe pipe;
	enum hdcp_type hdcp_type;
} data_t;

static igt_output_t *get_hdcp_output(data_t *data, uint32_t connector_id, bool *is_valid)
{
	drmModeConnector *connector = drmModeGetConnectorCurrent(data->fd, connector_id);
	igt_output_t *output;

	*is_valid = false;
	if (!connector)
		return NULL;

	output = igt_output_from_connector(&data->display, connector);
	if (!output) {
		drmModeFreeConnector(connector);
		return NULL;
	}

	if (connector->connection != DRM_MODE_CONNECTED || connector->encoder_id == 0) {
		drmModeFreeConnector(connector);
		return NULL;
	}

	drmModeFreeConnector(connector);
	*is_valid = true;
	return output;
}

static const char *get_hdcp_version(int fd, char *connector_name)
{
	char buf[MAX_HDCP_BUF_LEN];
	int ret;

	ret = igt_debugfs_connector_dir(fd, connector_name, O_RDONLY);
	if (ret < 0) {
		fprintf(stderr, "Failed to open connector directory\n");
		return NULL;
	}

	if (is_intel_device(fd))
		igt_debugfs_simple_read(ret, "i915_hdcp_sink_capability", buf, sizeof(buf));
	else
		igt_debugfs_simple_read(ret, "hdcp_sink_capability", buf, sizeof(buf));

	close(ret);
	if (strstr(buf, "HDCP1.4") && strstr(buf, "HDCP2.2"))
		return "HDCP1.4 and HDCP2.2";
	else if (strstr(buf, "HDCP1.4"))
		return "HDCP1.4";
	else if (strstr(buf, "HDCP2.2"))
		return "HDCP2.2";
	else
		return "No HDCP support";
}

static void get_hdcp_info(data_t *data)
{
	char *output_name;
	drmModeRes *res = drmModeGetResources(data->fd);

	if (!res) {
		fprintf(stderr, "Failed to get DRM resources\n");
		return;
	}

	fprintf(stderr, "Connectors:\n");
	fprintf(stderr, "id\tencoder\tstatus\t\ttype\tHDCP\n");
	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *connector;

		connector = drmModeGetConnectorCurrent(data->fd, res->connectors[i]);

		if (!connector)
			continue;

		asprintf(&output_name, "%s-%d",
			 kmstest_connector_type_str(connector->connector_type),
			 connector->connector_type_id);

		fprintf(stderr, "%d\t%d\t%s\t%s\t%s\n",
			connector->connector_id, connector->encoder_id,
			kmstest_connector_status_str(connector->connection),
			kmstest_connector_type_str(connector->connector_type),
			get_hdcp_version(data->fd, output_name));

		drmModeFreeConnector(connector);
	}

	drmModeFreeResources(res);
}

static void print_usage(void)
{
	fprintf(stderr, "Usage: intel_hdcp [OPTIONS]\n");
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "-i,	--info		Get HDCP Information\n");
	fprintf(stderr, "-h,	--help		Display this help message\n");
}

static void test_init(data_t *data)
{
	drmModeModeInfo *mode;

	data->fd = __drm_open_driver(DRIVER_ANY);
	if (data->fd < 0) {
		fprintf(stderr, "Failed to open DRM driver\n");
		exit(EXIT_FAILURE);
	}
	igt_display_require(&data->display, data->fd);
	igt_display_require_output(&data->display);
	igt_display_reset(&data->display);

	for_each_pipe_with_valid_output(&data->display, data->pipe, data->output) {
		if (!igt_output_has_prop(data->output, IGT_CONNECTOR_CONTENT_PROTECTION))
			continue;

		mode = igt_output_get_mode(data->output);
		data->width = mode->hdisplay;
		data->height = mode->vdisplay;

		igt_create_color_fb(data->fd, data->width, data->height, DRM_FORMAT_XRGB8888,
				    DRM_FORMAT_MOD_LINEAR, 0.0, 0.0, 0.0, &data->fb);

		igt_output_set_crtc(data->output, igt_crtc_for_pipe(&data->display, data->pipe));
		igt_display_commit2(&data->display, COMMIT_ATOMIC);

		break;
	}
	igt_display_commit2(&data->display, COMMIT_ATOMIC);
}

static void cleanup(data_t *data)
{
	igt_remove_fb(data->fd, &data->fb);
	igt_display_fini(&data->display);
	close(data->fd);
}

int main(int argc, char **argv)
{
	data_t data;
	int option;
	static const char optstr[] = "hi";
	struct option long_opts[] = {
		{"help",	no_argument,	NULL, 'h'},
		{"info",	no_argument,	NULL, 'i'},
		{NULL,		0,		NULL,  0 }
	};

	test_init(&data);

	while ((option = getopt_long(argc, argv, optstr, long_opts, NULL)) != -1) {
		switch (option) {
		case 'i':
			get_hdcp_info(&data);
			break;
		case 'h':
		default:
			print_usage();
			break;
		}
	}
	cleanup(&data);
}
