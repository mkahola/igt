// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include <fcntl.h>
#include <stdio.h>
#include <ctype.h>

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
	int user_cmd;
	bool running;
	pthread_mutex_t lock;
	pthread_t video_tid;
	int selected_connector;
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

static const char * const menu_lines[] = {
	"=== HDCP Tool ===",
	"1. Get HDCP Information",
	"2. Enable HDCP1.4",
	"3. Enable HDCP2.2 Type 0",
	"4. Enable HDCP2.2 Type 1",
	"5. Disable HDCP",
	"q. Quit"
};

static void set_hdcp_prop(data_t *data, int property, int type, int connector_id)
{
	igt_output_t *output;
	bool is_valid;

	output = get_hdcp_output(data, connector_id, &is_valid);
	if (!output || !is_valid) {
		fprintf(stderr, "Invalid output or connector\n");
		return;
	}

	switch (property) {
	case CP_UNDESIRED:
		igt_output_set_prop_value(output, IGT_CONNECTOR_CONTENT_PROTECTION, CP_UNDESIRED);
		break;
	case CP_DESIRED:
		igt_output_set_prop_value(output, IGT_CONNECTOR_CONTENT_PROTECTION, CP_DESIRED);
		switch (type) {
		case HDCP_TYPE_1_4:
		case HDCP_TYPE_2_2_TYPE_0:
			igt_output_set_prop_value(output, IGT_CONNECTOR_HDCP_CONTENT_TYPE, 0);
			break;
		case HDCP_TYPE_2_2_TYPE_1:
			igt_output_set_prop_value(output, IGT_CONNECTOR_HDCP_CONTENT_TYPE, 1);
			break;
		default:
			fprintf(stderr, "Invalid HDCP type\n");
			return;
		}
		break;
	default:
		fprintf(stderr, "Invalid property value\n");
		return;
	}

	igt_display_commit2(&data->display, COMMIT_ATOMIC);
}

static const char *get_hdcp_content_type(data_t *data, uint32_t connector_id)
{
	igt_output_t *output;
	bool is_valid;
	uint64_t status, content_type;

	output = get_hdcp_output(data, connector_id, &is_valid);
	if (!output || !is_valid)
		return "N/A";

	status = igt_output_get_prop(output, IGT_CONNECTOR_CONTENT_PROTECTION);
	if (status != CP_ENABLED)
		return "N/A";

	content_type = igt_output_get_prop(output, IGT_CONNECTOR_HDCP_CONTENT_TYPE);
	switch (content_type) {
	case 0:
		return "Type 0";
	case 1:
		return "Type 1";
	default:
		return "Unknown";
	}
}

static const char *get_hdcp_status(data_t *data, uint32_t connector_id)
{
	igt_output_t *output;
	bool is_valid;
	uint64_t prop_value;

	output = get_hdcp_output(data, connector_id, &is_valid);
	if (!output || !is_valid)
		return "N/A";

	prop_value = igt_output_get_prop(output, IGT_CONNECTOR_CONTENT_PROTECTION);
	switch (prop_value) {
	case CP_UNDESIRED:
		return "Disabled";
	case CP_DESIRED:
		return "Desired";
	case CP_ENABLED:
		return "Enabled";
	default:
		return "Unknown";
	}
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
	fprintf(stderr, "%-4s %-8s %-12s %-8s %-20s %-12s %-12s\n",
		"ID", "Encoder", "Status", "Type", "HDCP Support", "HDCP Status", "Content Type");
	fprintf(stderr, "---- -------- ------------ -------- -------------------- ------------\n");

	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *connector;

		connector = drmModeGetConnectorCurrent(data->fd, res->connectors[i]);

		if (!connector)
			continue;

		asprintf(&output_name, "%s-%d",
			 kmstest_connector_type_str(connector->connector_type),
			 connector->connector_type_id);

		fprintf(stderr, "%-4d %-8d %-12s %-8s %-20s %-12s %-12s\n",
			connector->connector_id, connector->encoder_id,
			kmstest_connector_status_str(connector->connection),
			kmstest_connector_type_str(connector->connector_type),
			get_hdcp_version(data->fd, output_name),
			get_hdcp_status(data, connector->connector_id),
			get_hdcp_content_type(data, connector->connector_id));

		free(output_name);
		drmModeFreeConnector(connector);
	}
	fprintf(stderr, "---- -------- ------------ -------- -------------------- ------------\n");

	drmModeFreeResources(res);
}

static void print_usage(void)
{
	fprintf(stderr, "Usage: intel_hdcp [OPTIONS]\n");
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "-i,	--info		Get HDCP Information\n");
	fprintf(stderr, "-h,	--help		Display this help message\n");
}

static void print_menu(void)
{
	printf("\n");
	for (size_t i = 0; i < ARRAY_SIZE(menu_lines); i++)
		printf("%s\n", menu_lines[i]);
	printf("\n");
}

static bool validate_input(const char *input, char *choice)
{
	const char *p = input;
	const char *rest;

	/* Trim leading whitespace */
	while (*p && isspace((unsigned char)*p))
		p++;

	/* Empty input */
	if (*p == '\0')
		return false;

	*choice = *p;
	rest = p + 1;

	/* Ensure rest is only whitespace/newline */
	while (*rest) {
		if (!isspace((unsigned char)*rest) && *rest != '\0') {
			fprintf(stderr, "Invalid input format\n");
			return false;
		}
		rest++;
	}

	return true;
}

static int get_valid_hdcp_connectors(data_t *data, int *valid_connectors)
{
	drmModeRes *res;
	int valid_count = 0;

	res = drmModeGetResources(data->fd);
	if (!res)
		return 0;

	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *connector;
		char *output_name;
		const char *hdcp_support;

		connector = drmModeGetConnectorCurrent(data->fd, res->connectors[i]);
		if (!connector)
			continue;

		if (connector->connection == DRM_MODE_CONNECTED &&
		    connector->encoder_id != 0) {
			asprintf(&output_name, "%s-%d",
				 kmstest_connector_type_str(connector->connector_type),
				 connector->connector_type_id);
			hdcp_support = get_hdcp_version(data->fd, output_name);
			free(output_name);

			if (hdcp_support && strcmp(hdcp_support, "No HDCP support") != 0)
				valid_connectors[valid_count++] = connector->connector_id;
		}
		drmModeFreeConnector(connector);
	}
	drmModeFreeResources(res);

	return valid_count;
}

static bool select_connector(data_t *data, int *valid_connectors, int valid_count)
{
	char input[256];
	int cid;
	const char *q;

	printf("\nValid HDCP-capable connector IDs: ");
	for (int i = 0; i < valid_count; i++) {
		printf("%d", valid_connectors[i]);
		if (i < valid_count - 1)
			printf(", ");
	}
	printf("\n");

	while (data->running) {
		printf("Enter connector id (blank to cancel): ");
		fflush(stdout);

		if (!fgets(input, sizeof(input), stdin)) {
			fprintf(stderr, "No input; aborting action\n");
			return false;
		}

		/* Check for blank line (cancel) */
		q = input;
		while (*q && isspace((unsigned char)*q))
			q++;

		if (*q == '\0' || *q == '\n') {
			fprintf(stderr, "Cancelled connector selection\n");
			return false;
		}

		if (sscanf(input, "%d", &cid) != 1) {
			fprintf(stderr, "Invalid number; try again\n");
			continue;
		}

		/* Verify connector is in valid list */
		for (int i = 0; i < valid_count; i++) {
			if (valid_connectors[i] == cid) {
				data->selected_connector = cid;
				printf("Using connector: %d\n", data->selected_connector);
				return true;
			}
		}

		fprintf(stderr, "Connector %d is not a valid HDCP-capable connector\n", cid);
	}

	return false;
}

static bool handle_connector_selection(data_t *data, int cmd)
{
	int valid_connectors[32];
	int valid_count;

	/* Commands 2-5 need connector selection if not already set */
	if (cmd < 2 || cmd > 5 || data->selected_connector != -1)
		return true;

	valid_count = get_valid_hdcp_connectors(data, valid_connectors);

	if (valid_count == 0) {
		fprintf(stderr, "No HDCP-capable connectors available\n");
		return false;
	}

	return select_connector(data, valid_connectors, valid_count);
}

static void *keypress_thread(void *arg)
{
	data_t *data = (data_t *)arg;
	char input[256], choice;

	print_menu();

	while (data->running) {
		printf("Enter input 1-5 or q: ");
		fflush(stdout);

		if (!fgets(input, sizeof(input), stdin)) {
			printf("\nExiting...\n");
			break;
		}

		if (!validate_input(input, &choice))
			continue;

		pthread_mutex_lock(&data->lock);

		if (choice == 'q' || choice == 'Q') {
			data->running = false;
			pthread_mutex_unlock(&data->lock);
			continue;
		}

		if (choice >= '1' && choice <= '5') {
			data->user_cmd = choice - '0';

			if (!handle_connector_selection(data, data->user_cmd))
				data->user_cmd = 0;
		} else {
			fprintf(stderr, "Invalid choice: %c\n", choice);
		}

		pthread_mutex_unlock(&data->lock);
	}

	return NULL;
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
