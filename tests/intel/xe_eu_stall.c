// SPDX-License-Identifier: MIT
/*
 * Copyright(c) 2025 Intel Corporation. All rights reserved.
 */

/**
 * TEST: Basic tests for EU stall sampling functionality
 * Category: Core
 * Functionality: EU stall sampling
 * Mega feature: Performance interface
 * Sub-category: Performance
 * Test category: xe
 *
 * SUBTEST: non-blocking-read
 * Description: Verify non-blocking read of EU stall data during a workload run
 *
 * SUBTEST: non-blocking-re-enable
 * Description: Run non-blocking read test twice with disable and enable between the runs
 *
 * SUBTEST: blocking-read
 * Description: Verify blocking read of EU stall data during a workload run
 *
 * SUBTEST: blocking-re-enable
 * Description: Run blocking read test twice with disable and enable between the runs
 *
 * SUBTEST: unprivileged-access
 * Description: Verify unprivileged open of a EU stall data stream fd
 *
 * SUBTEST: invalid-gt-id
 * Description: Verify that invalid input GT ID fails the test
 *
 * SUBTEST: invalid-sampling-rate
 * Description: Verify that invalid input sampling rate fails the test
 *
 * SUBTEST: invalid-event-report-count
 * Description: Verify that invalid input event report count fails the test
 */

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "igt.h"
#include "igt_core.h"
#include "xe_drm.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"

#define OBSERVATION_PARANOID	"/proc/sys/dev/xe/observation_paranoid"

#define NUM_DATA_ROWS(size)	((size) >> 6)

#define MAX_XECORES		64
#define NUM_ITERS_GPGPU_FILL	100
#define DEFAULT_NUM_REPORTS	1
#define DEFAULT_USER_BUF_SIZE	(64 * 512 * 1024)

#define WIDTH		64
#define HEIGHT		64
#define COLOR_88	0x88
#define COLOR_4C	0x4c

static FILE *output;
static char *p_args[8];
static char *output_file;
static uint8_t *user_buf;
static uint8_t p_gt_id;
static uint32_t p_rate;
static uint32_t p_user = DEFAULT_USER_BUF_SIZE;
static uint32_t p_num_reports = DEFAULT_NUM_REPORTS;
static int stream_fd = -1;

static volatile bool child_is_running = true;

static struct drm_xe_query_eu_stall *query_eu_stall_data;

/*
 * EU stall data format for PVC
 */
struct xe_eu_stall_data_pvc {
	__u64 ip_addr:29;	  /* Bits 0  to 28  */
	__u64 active_count:8;	  /* Bits 29 to 36  */
	__u64 other_count:8;	  /* Bits 37 to 44  */
	__u64 control_count:8;	  /* Bits 45 to 52  */
	__u64 pipestall_count:8;  /* Bits 53 to 60  */
	__u64 send_count:8;	  /* Bits 61 to 68  */
	__u64 dist_acc_count:8;	  /* Bits 69 to 76  */
	__u64 sbid_count:8;	  /* Bits 77 to 84  */
	__u64 sync_count:8;	  /* Bits 85 to 92  */
	__u64 inst_fetch_count:8; /* Bits 93 to 100 */
	__u64 unused_bits:27;
	__u64 unused[6];
} __attribute__((packed));

/*
 * EU stall data format for Xe2 arch GPUs (LNL, BMG).
 */
struct xe_eu_stall_data_xe2 {
	__u64 ip_addr:29;	  /* Bits 0  to 28  */
	__u64 tdr_count:8;	  /* Bits 29 to 36  */
	__u64 other_count:8;	  /* Bits 37 to 44  */
	__u64 control_count:8;	  /* Bits 45 to 52  */
	__u64 pipestall_count:8;  /* Bits 53 to 60  */
	__u64 send_count:8;	  /* Bits 61 to 68  */
	__u64 dist_acc_count:8;   /* Bits 69 to 76  */
	__u64 sbid_count:8;	  /* Bits 77 to 84  */
	__u64 sync_count:8;	  /* Bits 85 to 92  */
	__u64 inst_fetch_count:8; /* Bits 93 to 100 */
	__u64 active_count:8;	  /* Bits 101 to 108 */
	__u64 ex_id:3;		  /* Bits 109 to 111 */
	__u64 end_flag:1;	  /* Bit  112 */
	__u64 unused_bits:15;
	__u64 unused[6];
} __attribute__((packed));

struct xe_eu_stall_open_prop {
	uint32_t num_properties;
	uint32_t reserved;
	uint64_t properties_ptr;
};

union xe_eu_stall_data {
	struct xe_eu_stall_data_pvc pvc;
	struct xe_eu_stall_data_xe2 xe2;
};

typedef struct {
	int drm_fd;
	uint32_t devid;
	struct buf_ops *bops;
} data_t;

static struct intel_buf *
create_buf(data_t *data, int width, int height, uint8_t color, uint64_t region)
{
	struct intel_buf *buf;
	uint8_t *ptr;
	int i;

	buf = calloc(1, sizeof(*buf));
	igt_assert(buf);

	buf = intel_buf_create(data->bops, width / 4, height, 32, 0,
			       I915_TILING_NONE, 0);

	ptr = xe_bo_map(data->drm_fd, buf->handle, buf->surface[0].size);

	for (i = 0; i < buf->surface[0].size; i++)
		ptr[i] = color;

	munmap(ptr, buf->surface[0].size);

	return buf;
}

static void buf_check(uint8_t *ptr, int width, int x, int y, uint8_t color)
{
	uint8_t val;

	val = ptr[y * width + x];
	igt_assert_f(val == color,
		     "Expected 0x%02x, found 0x%02x at (%d,%d)\n",
		     color, val, x, y);
}

static void gpgpu_fill(data_t *data, igt_fillfunc_t fill, uint32_t region,
		       uint32_t surf_width, uint32_t surf_height,
		       uint32_t x, uint32_t y,
		       uint32_t width, uint32_t height)
{
	struct intel_buf *buf;
	uint8_t *ptr;
	int i, j;

	buf = create_buf(data, surf_width, surf_height, COLOR_88, region);
	ptr = xe_bo_map(data->drm_fd, buf->handle, buf->surface[0].size);

	for (i = 0; i < surf_width; i++)
		for (j = 0; j < surf_height; j++)
			buf_check(ptr, surf_width, i, j, COLOR_88);

	fill(data->drm_fd, buf, x, y, width, height, COLOR_4C);

	for (i = 0; i < surf_width; i++)
		for (j = 0; j < surf_height; j++)
			if (i >= x && i < width + x &&
			    j >= y && j < height + y)
				buf_check(ptr, surf_width, i, j, COLOR_4C);
			else
				buf_check(ptr, surf_height, i, j, COLOR_88);

	munmap(ptr, buf->surface[0].size);
}

static int run_gpgpu_fill(int drm_fd, uint32_t devid)
{
	data_t data = {drm_fd, devid, NULL};
	igt_fillfunc_t fill_fn = NULL;
	unsigned int i;

	data.bops = buf_ops_create(drm_fd);
	fill_fn = igt_get_gpgpu_fillfunc(devid);

	for (i = 0; i < NUM_ITERS_GPGPU_FILL; i++)
		gpgpu_fill(&data, fill_fn, 0, WIDTH, HEIGHT, 16, 16, WIDTH / 2, HEIGHT / 2);

	buf_ops_destroy(data.bops);

	return EXIT_SUCCESS;
}

/**
 * xe_eu_stall_prop_to_ext:
 * @properties: pointer to internal IGT properties
 * @extn: Pointer to array of set_property uapi structs
 *
 * Convert 'struct xe_eu_stall_open_prop' properties used internally in IGT
 * into chained 'struct drm_xe_ext_set_property' structures used in
 * EU stall sampling uapi
 */
static void xe_eu_stall_prop_to_ext(struct xe_eu_stall_open_prop *properties,
				    struct drm_xe_ext_set_property *extn)
{
	__u64 *prop = from_user_pointer(properties->properties_ptr);
	struct drm_xe_ext_set_property *ext = extn;
	int i, j;

	for (i = 0; i < properties->num_properties; i++) {
		ext->base.name = DRM_XE_EU_STALL_EXTENSION_SET_PROPERTY;
		ext->property = *prop++;
		ext->value = *prop++;
		ext++;
	}

	igt_assert_lte(1, i);
	ext = extn;
	for (j = 0; j < i - 1; j++)
		ext[j].base.next_extension = to_user_pointer(&ext[j + 1]);
}

static int xe_eu_stall_ioctl(int fd, enum drm_xe_observation_op op, void *arg)
{
#define XE_EU_STALL_MAX_SET_PROPERTIES 5

	struct drm_xe_ext_set_property ext[XE_EU_STALL_MAX_SET_PROPERTIES] = {};

	/* Chain the PERF layer struct */
	struct drm_xe_observation_param p = {
		.extensions = 0,
		.observation_type = DRM_XE_OBSERVATION_TYPE_EU_STALL,
		.observation_op = op,
		.param = to_user_pointer((op == DRM_XE_OBSERVATION_OP_STREAM_OPEN) ? ext : arg),
	};

	if (op == DRM_XE_OBSERVATION_OP_STREAM_OPEN) {
		struct xe_eu_stall_open_prop *oprop = (struct xe_eu_stall_open_prop *)arg;

		igt_assert_lte(oprop->num_properties, XE_EU_STALL_MAX_SET_PROPERTIES);
		xe_eu_stall_prop_to_ext(oprop, ext);
	}

	return igt_ioctl(fd, DRM_IOCTL_XE_OBSERVATION, &p);
}

static void xe_eu_stall_ioctl_err(int fd, enum drm_xe_observation_op op, void *arg, int err)
{
	igt_assert_eq(xe_eu_stall_ioctl(fd, op, arg), -1);
	igt_assert_eq(errno, err);
	errno = 0;
}

static uint64_t read_u64_file(const char *path)
{
	FILE *f;
	uint64_t val;

	f = fopen(path, "r");
	igt_assert(f);

	igt_assert_eq(fscanf(f, "%"PRIu64, &val), 1);

	fclose(f);

	return val;
}

static void write_u64_file(const char *path, uint64_t val)
{
	FILE *f;

	f = fopen(path, "w");
	igt_assert(f);

	igt_assert(fprintf(f, "%"PRIu64, val) > 0);

	fclose(f);
}

static void set_fd_flags(int fd, int flags)
{
	int old = fcntl(fd, F_GETFL, 0);

	igt_assert_lte(0, old);
	igt_assert_eq(0, fcntl(fd, F_SETFL, old | flags));
}

static void eu_stall_close(int fd)
{
	close(fd);
	stream_fd = -1;
}

static int eu_stall_open(int drm_fd, struct xe_eu_stall_open_prop *props)
{
	int ret;

	if (stream_fd >= 0)
		eu_stall_close(stream_fd);

	ret = xe_eu_stall_ioctl(drm_fd, DRM_XE_OBSERVATION_OP_STREAM_OPEN, props);
	igt_assert_fd(ret);

	return ret;
}

/*
 * Verify that tests with invalid arguments fail.
 */
static void test_invalid_arguments(int drm_fd, uint8_t gt_id, uint32_t rate, uint32_t num_reports)
{
	uint64_t properties[] = {
		DRM_XE_EU_STALL_PROP_GT_ID, gt_id,
		DRM_XE_EU_STALL_PROP_SAMPLE_RATE, rate,
		DRM_XE_EU_STALL_PROP_WAIT_NUM_REPORTS, num_reports,
	};

	struct xe_eu_stall_open_prop props = {
		.num_properties = ARRAY_SIZE(properties) / 2,
		.properties_ptr = to_user_pointer(properties),
	};

	xe_eu_stall_ioctl_err(drm_fd, DRM_XE_OBSERVATION_OP_STREAM_OPEN, &props, EINVAL);
}

static void test_invalid_gt_id(int fd)
{
	test_invalid_arguments(fd, 255, p_rate, DEFAULT_NUM_REPORTS);
}

static void test_invalid_sampling_rate(int fd)
{
	test_invalid_arguments(fd, 0, p_rate * 10, DEFAULT_NUM_REPORTS);
}

static void test_invalid_event_report_count(int fd)
{
	test_invalid_arguments(fd, 0, p_rate,
			       NUM_DATA_ROWS(512 * 1024) * MAX_XECORES + 1);
}

static inline void enable_paranoid(void)
{
	write_u64_file(OBSERVATION_PARANOID, 1);
}

static inline void disable_paranoid(void)
{
	write_u64_file(OBSERVATION_PARANOID, 0);
}

/*
 * Test to verify that only a privileged process can open
 * an EU stall data stream file descriptor.
 */
static void test_non_privileged_access(int drm_fd)
{
	int paranoid;

	/* Close any open stream fd before fork() */
	if (stream_fd >= 0)
		eu_stall_close(stream_fd);

	paranoid = read_u64_file(OBSERVATION_PARANOID);

	igt_fork(child, 1) {
		uint64_t properties[] = {
			DRM_XE_EU_STALL_PROP_GT_ID, p_gt_id,
			DRM_XE_EU_STALL_PROP_SAMPLE_RATE, p_rate,
			DRM_XE_EU_STALL_PROP_WAIT_NUM_REPORTS, p_num_reports,
		};

		struct xe_eu_stall_open_prop props = {
			.num_properties = ARRAY_SIZE(properties) / 2,
			.properties_ptr = to_user_pointer(properties),
		};

		if (!paranoid)
			enable_paranoid();

		igt_drop_root();

		xe_eu_stall_ioctl_err(drm_fd, DRM_XE_OBSERVATION_OP_STREAM_OPEN, &props, EACCES);
	}

	igt_waitchildren();

	igt_fork(child, 1) {
		uint64_t properties[] = {
			DRM_XE_EU_STALL_PROP_GT_ID, p_gt_id,
			DRM_XE_EU_STALL_PROP_SAMPLE_RATE, p_rate,
			DRM_XE_EU_STALL_PROP_WAIT_NUM_REPORTS, p_num_reports,
		};

		struct xe_eu_stall_open_prop props = {
			.num_properties = ARRAY_SIZE(properties) / 2,
			.properties_ptr = to_user_pointer(properties),
		};

		disable_paranoid();

		igt_drop_root();

		stream_fd = eu_stall_open(drm_fd, &props);
		eu_stall_close(stream_fd);
	}

	igt_waitchildren();

	/* restore paranoid state */
	if (paranoid)
		enable_paranoid();
}

static int wait_child(struct igt_helper_process *child_proc)
{
	int status;

	status = igt_wait_helper(child_proc);
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return (128 + WTERMSIG(status));
	return 0;
}

static void sighandler(int sig)
{
	child_is_running = false;
}

static void print_eu_stall_data(uint32_t devid, uint8_t *buf, size_t size)
{
	int i;
	uint8_t *sample_addr;
	union xe_eu_stall_data stall_data;

	for (i = 0; i < size / sizeof(stall_data); i++) {
		sample_addr = buf + (i * sizeof(stall_data));
		memcpy(&stall_data, sample_addr, sizeof(stall_data));
		if (IS_PONTEVECCHIO(devid)) {
			fprintf(output, "ip: 0x%08x ", stall_data.pvc.ip_addr);
			fprintf(output, "active: %u ", stall_data.pvc.active_count);
			fprintf(output, "other: %u ", stall_data.pvc.other_count);
			fprintf(output, "control: %u ", stall_data.pvc.control_count);
			fprintf(output, "pipestall: %u ", stall_data.pvc.pipestall_count);
			fprintf(output, "send: %u ", stall_data.pvc.send_count);
			fprintf(output, "dist_acc: %u ", stall_data.pvc.dist_acc_count);
			fprintf(output, "sbid: %u ", stall_data.pvc.sbid_count);
			fprintf(output, "sync: %u ", stall_data.pvc.sync_count);
			fprintf(output, "inst_fetch: %u\n", stall_data.pvc.inst_fetch_count);
		} else {
			fprintf(output, "ip: 0x%08x ", stall_data.xe2.ip_addr);
			fprintf(output, "tdr: %u ", stall_data.xe2.tdr_count);
			fprintf(output, "other: %u ", stall_data.xe2.other_count);
			fprintf(output, "control: %u ", stall_data.xe2.control_count);
			fprintf(output, "pipestall: %u ", stall_data.xe2.pipestall_count);
			fprintf(output, "send: %u ", stall_data.xe2.send_count);
			fprintf(output, "dist_acc: %u ", stall_data.xe2.dist_acc_count);
			fprintf(output, "sbid: %u ", stall_data.xe2.sbid_count);
			fprintf(output, "sync: %u ", stall_data.xe2.sync_count);
			fprintf(output, "inst_fetch: %u ", stall_data.xe2.inst_fetch_count);
			fprintf(output, "active: %u ", stall_data.xe2.active_count);
			fprintf(output, "ex_id: %u ", stall_data.xe2.ex_id);
			fprintf(output, "end_flag: %u\n", stall_data.xe2.end_flag);
		}
	}
}

/*
 * Test enables EU stall counters, runs a given workload on a child process
 * while the parent process reads the stall counters data, disables EU stall
 * counters once the workload completes execution.
 */
static void test_eustall(int drm_fd, uint32_t devid, bool blocking_read, int iter)
{
	uint32_t num_samples, num_drops;
	struct igt_helper_process work_load = {};
	struct sigaction sa = { 0 };
	int ret, flags;
	uint64_t total_size;

	uint64_t properties[] = {
		DRM_XE_EU_STALL_PROP_GT_ID, p_gt_id,
		DRM_XE_EU_STALL_PROP_SAMPLE_RATE, p_rate,
		DRM_XE_EU_STALL_PROP_WAIT_NUM_REPORTS, p_num_reports,
	};

	struct xe_eu_stall_open_prop props = {
		.num_properties = ARRAY_SIZE(properties) / 2,
		.properties_ptr = to_user_pointer(properties),
	};

	igt_info("User buffer size: %u\n", p_user);
	if (p_args[0])
		igt_info("Workload: %s\n", p_args[0]);
	else
		igt_info("Workload: GPGPU fill\n");

	igt_info("Sampling Rate: %u\n", p_rate);

	stream_fd = eu_stall_open(drm_fd, &props);

	if (!blocking_read)
		flags = O_CLOEXEC | O_NONBLOCK;
	else
		flags = O_CLOEXEC;

	set_fd_flags(stream_fd, flags);
enable:
	do_ioctl(stream_fd, DRM_XE_OBSERVATION_IOCTL_ENABLE, 0);

	sa.sa_handler = sighandler;
	if (sigaction(SIGCHLD, &sa, NULL) == -1) {
		igt_critical("Failed to register SIGCHLD signal handler\n");
		igt_fail(IGT_EXIT_FAILURE);
	}

	child_is_running = true;
	/* Child process runs the workload */
	igt_fork_helper(&work_load) {
		setpgid(0, 0);
		if (p_args[0]) {
			execv(p_args[0], p_args);
			_exit(EXIT_FAILURE);
		} else {
			_exit(run_gpgpu_fill(drm_fd, devid));
		}
	}
	total_size = 0;
	num_samples = 0;
	num_drops = 0;
	/* Parent process reads the EU stall counters data */
	do {
		if (!blocking_read) {
			struct pollfd pollfd = { .fd = stream_fd, .events = POLLIN };

			ret = poll(&pollfd, 1, 0);
			if (ret <= 0)
				continue;
			igt_assert_eq(ret, 1);
			igt_assert(pollfd.revents & POLLIN);
		}
		ret = read(stream_fd, user_buf, p_user);
		if (ret > 0) {
			total_size += ret;
			if (output)
				print_eu_stall_data(devid, user_buf, ret);
			num_samples += ret / query_eu_stall_data->record_size;
		} else if ((ret < 0) && (errno != EAGAIN)) {
			if (errno == EINTR)
				continue;
			if (errno == EIO) {
				num_drops++;
				continue;
			}
			igt_critical("read() - ret: %d, errno: %d\n", ret, errno);
			kill(-work_load.pid, SIGTERM);
			break;
		}
	} while (child_is_running);

	do_ioctl(stream_fd, DRM_XE_OBSERVATION_IOCTL_DISABLE, 0);

	igt_info("Total size read: %" PRIu64 "\n", total_size);
	igt_info("Number of samples: %u\n", num_samples);
	igt_info("Number of drops reported: %u\n", num_drops);

	ret = wait_child(&work_load);
	igt_assert_f(ret == 0, "waitpid() - ret: %d, errno: %d\n", ret, errno);
	if (!igt_run_in_simulation())
		igt_assert_f(num_samples, "No EU stalls detected during the workload\n");

	if (--iter)
		goto enable;

	eu_stall_close(stream_fd);
}

static int opt_handler(int opt, int opt_index, void *data)
{
	switch (opt) {
	case 'e':
		p_num_reports = strtoul(optarg, NULL, 0);
		break;
	case 'g':
		p_gt_id = strtoul(optarg, NULL, 0);
		break;
	case 'o':
		output_file = optarg;
		break;
	case 'r':
		p_rate = strtoul(optarg, NULL, 0);
		break;
	case 'u':
		p_user = strtoul(optarg, NULL, 0);
		break;
	case 'w':
		p_args[0] = optarg;
		p_args[1] = NULL;
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

const char *help_str =  "  --event_count | -e\t\tPoll event report count\n"
			"  --gt_id | -g\t\tGT ID for the GT to sample EU stalls\n"
			"  --output | -o\t\tOutput file to write EU stall data\n"
			"  --rate | -r\t\tSampling rate in GPU cycles\n"
			"  --user_buf_sz | -u\t\tUser buffer size\n"
			"  --workload | -w\t\tWorkload to run\n";

static struct option long_options[] = {
	{"event_count", 0, 0, 'e'},
	{"gt_id", 0, 0, 'g'},
	{"output", 0, 0, 'o'},
	{"rate", 0, 0, 'r'},
	{"user_buf_sz", 0, 0, 'u'},
	{"workload", 0, 0, 'w'},
	{ NULL, 0, 0, 0 }
};

igt_main_args("e:g:o:r:u:w:", long_options, help_str, opt_handler, NULL)
{
	bool blocking_read = true;
	struct xe_device *xe_dev;
	int drm_fd;
	uint32_t devid;
	struct stat sb;

	igt_fixture() {
		drm_fd = drm_open_driver(DRIVER_XE);
		igt_require_fd(drm_fd);
		devid = intel_get_drm_devid(drm_fd);

		igt_require_f(igt_get_gpgpu_fillfunc(devid), "no gpgpu-fill function\n");
		igt_require_f(!stat(OBSERVATION_PARANOID, &sb), "no observation_paranoid file\n");
		xe_dev = xe_device_get(drm_fd);
		igt_require_f(xe_dev->eu_stall, "EU stall monitoring is not available/supported\n");

		query_eu_stall_data = xe_dev->eu_stall;
		igt_assert(query_eu_stall_data->num_sampling_rates > 0);
		/* If the user doesn't pass a sampling rate, use a mid sampling rate */
		if (p_rate == 0)
			p_rate = query_eu_stall_data->sampling_rates[0];

		if (output_file) {
			output = fopen(output_file, "w");
			igt_require(output);
		}
		user_buf = malloc(p_user);
		igt_assert(user_buf);
	}

	igt_describe("Verify non-blocking read of EU stall data during a workload run");
	igt_subtest("non-blocking-read") {
		test_eustall(drm_fd, devid, !blocking_read, 1);
	}

	igt_describe("Run non-blocking read test twice with disable and enable between the runs");
	igt_subtest("non-blocking-re-enable") {
		test_eustall(drm_fd, devid, !blocking_read, 2);
	}

	igt_describe("Verify blocking read of EU stall data during a workload run");
	igt_subtest("blocking-read") {
		test_eustall(drm_fd, devid, blocking_read, 1);
	}

	igt_describe("Run blocking read test twice with disable and enable between the runs");
	igt_subtest("blocking-re-enable") {
		test_eustall(drm_fd, devid, blocking_read, 2);
	}

	igt_describe("Verify that unprivileged open of a EU stall data fd fails");
	igt_subtest("unprivileged-access")
		test_non_privileged_access(drm_fd);

	igt_describe("Verify that invalid input GT ID fails the test");
	igt_subtest("invalid-gt-id")
		test_invalid_gt_id(drm_fd);

	igt_describe("Verify that invalid input sampling rate fails the test");
	igt_subtest("invalid-sampling-rate")
		test_invalid_sampling_rate(drm_fd);

	igt_describe("Verify that invalid input event report count fails the test");
	igt_subtest("invalid-event-report-count")
		test_invalid_event_report_count(drm_fd);

	igt_fixture() {
		free(user_buf);
		if (output)
			fclose(output);
		drm_close_driver(drm_fd);
	}
}
