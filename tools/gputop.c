// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <termios.h>
#include <sys/sysmacros.h>
#include <stdbool.h>

#include "igt_core.h"
#include "igt_drm_clients.h"
#include "igt_drm_fdinfo.h"
#include "igt_profiling.h"

enum utilization_type {
	UTILIZATION_TYPE_ENGINE_TIME,
	UTILIZATION_TYPE_TOTAL_CYCLES,
};

enum intel_driver_type {
	INTEL_DRIVER_I915,
	INTEL_DRIVER_XE,
	INTEL_DRIVER_UNKNOWN,
};

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

static const char *bars[] = { " ", "▏", "▎", "▍", "▌", "▋", "▊", "▉", "█" };

#define ANSI_HEADER "\033[7m"
#define ANSI_RESET "\033[0m"

static void n_spaces(const unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		putchar(' ');
}

static void print_percentage_bar(double percent, int max_len)
{
	int bar_len, i, len = max_len - 1;
	const int w = 8;

	len -= printf("|%5.1f%% ", percent);

	/* no space left for bars, do what we can */
	if (len < 0)
		len = 0;

	bar_len = ceil(w * percent * len / 100.0);
	if (bar_len > w * len)
		bar_len = w * len;

	for (i = bar_len; i >= w; i -= w)
		printf("%s", bars[w]);
	if (i)
		printf("%s", bars[i]);

	len -= (bar_len + (w - 1)) / w;
	n_spaces(len);

	putchar('|');
}

/* Get the correct sysfs prefix based on DRM minor number */
static const char *get_sysfs_drm_path(unsigned int drm_minor)
{
	return drm_minor >= 128 ? "/sys/class/drm/renderD" : "/sys/class/drm/card";
}

/* Detect if driver is bound to the device */
static bool is_bound(unsigned int drm_minor, const char *driver_name)
{
	char path[256];
	char link[256];
	char *driver;
	ssize_t len;

	/* Read the driver symlink */
	snprintf(path, sizeof(path), "%s%u/device/driver",
		 get_sysfs_drm_path(drm_minor), drm_minor);
	len = readlink(path, link, sizeof(link) - 1);
	if (len == -1)
		return false;

	link[len] = '\0';

	/* Extract driver name from path (e.g., "bus/pci/drivers/i915" -> "i915") */
	driver = strrchr(link, '/');
	if (!driver)
		return false;

	driver++; /* Skip the '/' */

	return strcmp(driver, driver_name) == 0;
}

/* Detect if this is an Intel device (i915 or Xe) */
static bool is_intel_device(unsigned int drm_minor)
{
	return is_bound(drm_minor, "i915") || is_bound(drm_minor, "xe");
}

/* Detect Intel driver variant */
static enum intel_driver_type detect_intel_driver(unsigned int drm_minor)
{
	if (is_bound(drm_minor, "xe"))
		return INTEL_DRIVER_XE;
	else if (is_bound(drm_minor, "i915"))
		return INTEL_DRIVER_I915;

	return INTEL_DRIVER_UNKNOWN;
}

/* Count the number of GTs by checking which gt* directories exist */
static int get_num_gts(unsigned int drm_minor, enum intel_driver_type driver_type)
{
	char path[256];
	struct stat st;
	int gt_count = 0;

	/* Check for gt0, gt1, gt2, etc. up to a reasonable maximum */
	for (int i = 0; i < 8; i++) {
		if (driver_type == INTEL_DRIVER_XE)
			/* For Xe, all GTs are under tile0 */
			snprintf(path, sizeof(path), "%s%u/device/tile0/gt%d",
				 get_sysfs_drm_path(drm_minor), drm_minor, i);
		else /* i915 */
			snprintf(path, sizeof(path), "%s%u/gt/gt%d",
				 get_sysfs_drm_path(drm_minor), drm_minor, i);

		if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
			gt_count++;
		else
			break;  /* Assume GTs are numbered sequentially */
	}

	return gt_count;
}

/* Read a single frequency value from sysfs */
static bool read_freq_value(const char *path, unsigned int *freq)
{
	FILE *fp;
	char buf[32];
	bool success = false;

	fp = fopen(path, "r");
	if (fp) {
		if (fgets(buf, sizeof(buf), fp)) {
			*freq = atoi(buf);
			success = true;
		}
		fclose(fp);
	}

	return success;
}

/* Intel-specific frequency reading for i915/Xe drivers */
static char *get_intel_frequencies(unsigned int drm_minor)
{
	char freq_str[512] = "";
	char gt_info[64];
	int num_gts;
	int gt;
	enum intel_driver_type driver_type;
	bool first = true;

	/* Detect Intel driver variant */
	driver_type = detect_intel_driver(drm_minor);
	if (driver_type == INTEL_DRIVER_UNKNOWN)
		return NULL;

	/* Get GT count from sysfs */
	num_gts = get_num_gts(drm_minor, driver_type);
	if (num_gts == 0)
		return NULL;

	/* Read frequencies for each GT */
	for (gt = 0; gt < num_gts; gt++) {
		char freq_path[256];
		unsigned int cur_freq = 0, act_freq = 0;
		bool has_cur = false, has_act = false;

		/* Read requested frequency */
		if (driver_type == INTEL_DRIVER_XE)
			snprintf(freq_path, sizeof(freq_path),
				 "%s%u/device/tile0/gt%d/freq0/cur_freq",
				 get_sysfs_drm_path(drm_minor), drm_minor, gt);
		else /* i915 */
			snprintf(freq_path, sizeof(freq_path),
				 "%s%u/gt/gt%d/rps_cur_freq_mhz",
				 get_sysfs_drm_path(drm_minor), drm_minor, gt);

		has_cur = read_freq_value(freq_path, &cur_freq);

		/* Read actual frequency */
		if (driver_type == INTEL_DRIVER_XE)
			snprintf(freq_path, sizeof(freq_path),
				 "%s%u/device/tile0/gt%d/freq0/act_freq",
				 get_sysfs_drm_path(drm_minor), drm_minor, gt);
		else /* i915 */
			snprintf(freq_path, sizeof(freq_path),
				 "%s%u/gt/gt%d/rps_act_freq_mhz",
				 get_sysfs_drm_path(drm_minor), drm_minor, gt);

		has_act = read_freq_value(freq_path, &act_freq);

		/* Skip this GT if we couldn't read any frequency */
		if (!has_cur && !has_act)
			continue;

		/* Append to frequency string */
		if (!first)
			strcat(freq_str, " ");
		else
			first = false;

		snprintf(gt_info, sizeof(gt_info), "GT%d-%u/%u", gt, cur_freq, act_freq);
		strcat(freq_str, gt_info);
	}

	if (strlen(freq_str) > 0)
		return strdup(freq_str);

	return NULL;
}

/* Generic wrapper for frequency reading - dispatches to vendor-specific implementations */
static char *get_frequencies(unsigned int drm_minor)
{
	/* Check for Intel devices (i915/Xe) */
	if (is_intel_device(drm_minor))
		return get_intel_frequencies(drm_minor);

	return NULL;
}

static int
print_client_header(struct igt_drm_client *c, int lines, int con_w, int con_h,
		    int *engine_w)
{
	int ret, len;
	char *freq_info = NULL;

	if (lines++ >= con_h)
		return lines;

	printf(ANSI_HEADER);

	/* Get frequency information */
	freq_info = get_frequencies(c->drm_minor);

	if (freq_info) {
		ret = printf("DRM minor %u   Frequency(MHz) %s", c->drm_minor, freq_info);
		free(freq_info);
	} else {
		ret = printf("DRM minor %u", c->drm_minor);
	}

	n_spaces(con_w - ret);

	if (lines++ >= con_h)
		return lines;

	putchar('\n');
	if (c->regions->num_regions)
		len = printf("%*s      MEM      RSS ",
			     c->clients->max_pid_len, "PID");
	else
		len = printf("%*s ", c->clients->max_pid_len, "PID");

	if (c->engines->num_engines) {
		unsigned int i;
		int width;

		*engine_w = width =
			(con_w - len - c->clients->max_name_len - 1) /
			c->engines->num_engines;

		for (i = 0; i <= c->engines->max_engine_id; i++) {
			const char *name = c->engines->names[i];
			int name_len = strlen(name);
			int pad = (width - name_len) / 2;
			int spaces = width - pad - name_len;

			if (!name)
				continue;

			if (pad < 0 || spaces < 0)
				continue;

			n_spaces(pad);
			printf("%s", name);
			n_spaces(spaces);
			len += pad + name_len + spaces;
		}
	}

	printf(" %-*s" ANSI_RESET "\n", con_w - len - 1, "NAME");

	return lines;
}

static bool
engines_identical(const struct igt_drm_client *c,
		  const struct igt_drm_client *pc)
{
	unsigned int i;

	if (c->engines->num_engines != pc->engines->num_engines ||
	    c->engines->max_engine_id != pc->engines->max_engine_id)
		return false;

	for (i = 0; i <= c->engines->max_engine_id; i++)
		if (c->engines->capacity[i] != pc->engines->capacity[i] ||
		    !!c->engines->names[i] != !!pc->engines->names[i] ||
		    strcmp(c->engines->names[i], pc->engines->names[i]))
			return false;

	return true;
}

static bool
newheader(const struct igt_drm_client *c, const struct igt_drm_client *pc)
{
	return !pc || c->drm_minor != pc->drm_minor ||
	       /*
		* Below is a a hack for drivers like amdgpu which omit listing
		* unused engines. Simply treat them as separate minors which
		* will ensure the per-engine columns are correctly sized in all
		* cases.
		*/
	       !engines_identical(c, pc);
}

static int
print_size(uint64_t sz)
{
	char units[] = {'B', 'K', 'M', 'G'};
	unsigned int u;

	for (u = 0; u < ARRAY_SIZE(units) - 1; u++) {
		if (sz < 1024)
			break;
		sz /= 1024;
	}

	return printf("%7"PRIu64"%c ", sz, units[u]);
}

static int
print_client(struct igt_drm_client *c, struct igt_drm_client **prevc,
	     double t, int lines, int con_w, int con_h,
	     unsigned int period_us, int *engine_w)
{
	enum utilization_type utilization_type;
	unsigned int i;
	uint64_t sz;
	int len;

	if (c->utilization_mask & IGT_DRM_CLIENT_UTILIZATION_TOTAL_CYCLES &&
	    c->utilization_mask & IGT_DRM_CLIENT_UTILIZATION_CYCLES)
		utilization_type = UTILIZATION_TYPE_TOTAL_CYCLES;
	else if (c->utilization_mask & IGT_DRM_CLIENT_UTILIZATION_ENGINE_TIME)
		utilization_type = UTILIZATION_TYPE_ENGINE_TIME;
	else
		return 0;

	if (c->samples < 2)
		return 0;

	/* Filter out idle clients. */
	switch (utilization_type) {
	case UTILIZATION_TYPE_ENGINE_TIME:
	       if (!c->total_engine_time)
		       return 0;
	       break;
	case UTILIZATION_TYPE_TOTAL_CYCLES:
	       if (!c->total_total_cycles)
		       return 0;
	       break;
	}

	/* Print header when moving to a different DRM card. */
	if (newheader(c, *prevc)) {
		lines = print_client_header(c, lines, con_w, con_h, engine_w);
		if (lines >= con_h)
			return lines;
	}

	*prevc = c;

	len = printf("%*s ", c->clients->max_pid_len, c->pid_str);

	if (c->regions->num_regions) {
		for (sz = 0, i = 0; i <= c->regions->max_region_id; i++)
			sz += c->memory[i].total;
		len += print_size(sz);

		for (sz = 0, i = 0; i <= c->regions->max_region_id; i++)
			sz += c->memory[i].resident;
		len += print_size(sz);
	}

	lines++;

	for (i = 0; c->samples > 1 && i <= c->engines->max_engine_id; i++) {
		double pct;

		if (!c->engines->capacity[i])
			continue;

		switch (utilization_type) {
		case UTILIZATION_TYPE_ENGINE_TIME:
			pct = (double)c->utilization[i].delta_engine_time / period_us / 1e3 * 100 /
				c->engines->capacity[i];
			break;
		case UTILIZATION_TYPE_TOTAL_CYCLES:
			pct = (double)c->utilization[i].delta_cycles / c->utilization[i].delta_total_cycles * 100 /
				c->engines->capacity[i];
			break;
		}

		/*
		 * Guard against fluctuations between our scanning period and
		 * GPU times as exported by the kernel in fdinfo.
		 */
		if (pct > 100.0)
			pct = 100.0;

		print_percentage_bar(pct, *engine_w);
		len += *engine_w;
	}

	printf(" %-*s\n", con_w - len - 1, c->print_name);

	return lines;
}

static int
__client_id_cmp(const struct igt_drm_client *a,
		const struct igt_drm_client *b)
{
	if (a->id > b->id)
		return 1;
	else if (a->id < b->id)
		return -1;
	else
		return 0;
}

static int client_cmp(const void *_a, const void *_b, void *unused)
{
	const struct igt_drm_client *a = _a;
	const struct igt_drm_client *b = _b;
	long val_a, val_b;

	/* DRM cards into consecutive buckets first. */
	val_a = a->drm_minor;
	val_b = b->drm_minor;
	if (val_a > val_b)
		return 1;
	else if (val_b > val_a)
		return -1;

	/*
	 * Within buckets sort by last sampling period aggregated runtime, with
	 * client id as a tie-breaker.
	 */
	val_a = a->agg_delta_engine_time;
	val_b = b->agg_delta_engine_time;
	if (val_a == val_b)
		return __client_id_cmp(a, b);
	else if (val_b > val_a)
		return 1;
	else
		return -1;

}

static void update_console_size(int *w, int *h)
{
	struct winsize ws = {};

	if (ioctl(0, TIOCGWINSZ, &ws) == -1)
		return;

	*w = ws.ws_col;
	*h = ws.ws_row;

	if (*w == 0 && *h == 0) {
		/* Serial console. */
		*w = 80;
		*h = 24;
	}
}

static void clrscr(void)
{
	printf("\033[H\033[J");
}

struct gputop_args {
	long n_iter;
	unsigned long delay_usec;
};

static void help(char *full_path)
{
	const char *short_program_name = strrchr(full_path, '/');

	if (short_program_name)
		short_program_name++;
	else
		short_program_name = full_path;

	printf("Usage:\n"
	       "\t%s [options]\n\n"
	       "Options:\n"
	       "\t-h, --help                show this help\n"
	       "\t-d, --delay =SEC[.TENTHS] iterative delay as SECS [.TENTHS]\n"
	       "\t-n, --iterations =NUMBER  number of executions\n"
	       , short_program_name);
}

static int parse_args(int argc, char * const argv[], struct gputop_args *args)
{
	static const char cmdopts_s[] = "hn:d:";
	static const struct option cmdopts[] = {
	       {"help", no_argument, 0, 'h'},
	       {"delay", required_argument, 0, 'd'},
	       {"iterations", required_argument, 0, 'n'},
	       { }
	};

	/* defaults */
	memset(args, 0, sizeof(*args));
	args->n_iter = -1;
	args->delay_usec = 2 * USEC_PER_SEC;

	for (;;) {
		int c, idx = 0;
		char *end_ptr = NULL;

		c = getopt_long(argc, argv, cmdopts_s, cmdopts, &idx);
		if (c == -1)
			break;

		switch (c) {
		case 'n':
			args->n_iter = strtol(optarg, NULL, 10);
			break;
		case 'd':
			args->delay_usec = strtoul(optarg, &end_ptr, 10) * USEC_PER_SEC;
			if (*end_ptr == '.')
				args->delay_usec += strtoul(end_ptr + 1, &end_ptr, 10) * USEC_PER_DECISEC;

			if (!args->delay_usec) {
				fprintf(stderr, "Invalid delay value: %s\n", optarg);
				return -1;
			}
			break;
		case 'h':
			help(argv[0]);
			return 0;
		default:
			fprintf(stderr, "Unkonwn option '%c'.\n", c);
			return -1;
		}
	}

	return 1;
}

static volatile bool stop_top;

static void sigint_handler(int sig)
{
	(void) sig;
	stop_top = true;
}

int main(int argc, char **argv)
{
	struct gputop_args args;
	unsigned int period_us;
	struct igt_profiled_device *profiled_devices = NULL;
	struct igt_drm_clients *clients = NULL;
	int con_w = -1, con_h = -1;
	int ret;
	long n;

	ret = parse_args(argc, argv, &args);
	if (ret < 0)
		return EXIT_FAILURE;
	if (!ret)
		return EXIT_SUCCESS;

	n = args.n_iter;
	period_us = args.delay_usec;

	clients = igt_drm_clients_init(NULL);
	if (!clients)
		exit(1);

	profiled_devices = igt_devices_profiled();
	if (profiled_devices != NULL) {
		igt_devices_configure_profiling(profiled_devices, true);

		if (signal(SIGINT, sigint_handler) == SIG_ERR) {
			fprintf(stderr, "Failed to install signal handler!\n");
			igt_devices_configure_profiling(profiled_devices, false);
			igt_devices_free_profiling(profiled_devices);
			profiled_devices = NULL;
		}
	}

	igt_drm_clients_scan(clients, NULL, NULL, 0, NULL, 0);

	while ((n != 0) && !stop_top) {
		struct igt_drm_client *c, *prevc = NULL;
		int i, engine_w = 0, lines = 0;

		igt_drm_clients_scan(clients, NULL, NULL, 0, NULL, 0);
		igt_drm_clients_sort(clients, client_cmp);

		update_console_size(&con_w, &con_h);
		clrscr();

		if (!clients->num_clients) {
			const char *msg = " (No GPU clients yet. Start workload to see stats)";

			printf(ANSI_HEADER "%-*s" ANSI_RESET "\n",
			       (int)(con_w - strlen(msg) - 1), msg);
		}

		igt_for_each_drm_client(clients, c, i) {
			assert(c->status != IGT_DRM_CLIENT_PROBE);
			if (c->status != IGT_DRM_CLIENT_ALIVE)
				break; /* Active clients are first in the array. */

			lines = print_client(c, &prevc, (double)period_us / 1e6,
					     lines, con_w, con_h, period_us,
					     &engine_w);
			if (lines >= con_h)
				break;
		}

		if (lines++ < con_h)
			printf("\n");

		usleep(period_us);
		if (n > 0)
			n--;

		if (profiled_devices != NULL)
			igt_devices_update_original_profiling_state(profiled_devices);
	}

	igt_drm_clients_free(clients);

	if (profiled_devices != NULL) {
		igt_devices_configure_profiling(profiled_devices, false);
		igt_devices_free_profiling(profiled_devices);
	}

	return 0;
}
