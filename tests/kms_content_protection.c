/*
 * Copyright © 2018 Intel Corporation
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
 * TEST: kms content protection
 * Category: Display
 * Description: Test content protection (HDCP)
 * Driver requirement: i915, xe
 * Mega feature: HDCP
 */

#include <poll.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <libudev.h>
#include "igt.h"
#include "igt_edid.h"
#include "igt_sysfs.h"
#include "igt_kms.h"
#include "igt_kmod.h"
#include "igt_panel.h"

/**
 * SUBTEST: lic-type-0
 * Description: Test for the integrity of link for type-0 content.
 *
 * SUBTEST: lic-type-1
 * Description: Test for the integrity of link for type-1 content.
 *
 * SUBTEST: content-type-change
 * Description: Test the content type change when the content protection already
 *              enabled
 *
 * SUBTEST: mei-interface
 * Description: Test the teardown and rebuild of the interface between Intel
 *              and mei hdcp.
 *
 * SUBTEST: srm
 * Description: This test writes the facsimile SRM into the /lib/firmware/ and
 *              check the kernel parsing of it by invoking the hdcp authentication.
 *
 * SUBTEST: uevent
 * Description: Test to detect the HDCP status change when we are reading the
 *              uevent sent with the corresponding connector id and property id.
 *
 * SUBTEST: %s
 * Description: Test content protection with %arg[1]
 *
 * arg[1]:
 *
 * @atomic:         atomic modesetting
 * @atomic-dpms:    DPMS ON/OFF during atomic modesetting.
 * @legacy:         legacy style commit
 * @type1:          content type 1 that can be handled only through HDCP2.2.
 * @suspend-resume: Suspend and resume the system
 */

/**
 * SUBTEST: dp-mst-%s
 * Description: Test Content protection %arg[1] over DP MST.
 *
 * arg[1]:
 *
 * @lic-type-0:   Type 0 with LIC
 * @lic-type-1:   Type 1 with LIC.
 * @type-0:       Type 0
 * @type-1:       Type 1
 * @suspend-resume: Suspend and resume the system
 */

IGT_TEST_DESCRIPTION("Test content protection (HDCP)");

struct data {
	int drm_fd;
	igt_display_t display;
	struct igt_fb red, green;
	unsigned int cp_tests;
	struct udev_monitor *uevent_monitor;
} data;

/* Test flags */
#define CP_DPMS					(1 << 0)
#define CP_LIC					(1 << 1)
#define CP_MEI_RELOAD				(1 << 2)
#define CP_TYPE_CHANGE				(1 << 3)
#define CP_UEVENT				(1 << 4)
#define SUSPEND_RESUME				(1 << 5)

#define CP_UNDESIRED				0
#define CP_DESIRED				1
#define CP_ENABLED				2

/*
 * HDCP_CONTENT_TYPE_0 can be handled on both HDCP1.4 and HDCP2.2. Where as
 * HDCP_CONTENT_TYPE_1 can be handled only through HDCP2.2.
 */
#define HDCP_CONTENT_TYPE_0				0
#define HDCP_CONTENT_TYPE_1				1

#define LIC_PERIOD_MSEC				(4 * 1000)
/* Kernel retry count=3, Max time per authentication allowed = 6Sec */
#define KERNEL_AUTH_TIME_ALLOWED_MSEC		(3 *  6 * 1000)
#define KERNEL_AUTH_TIME_ADDITIONAL_MSEC	100
#define KERNEL_DISABLE_TIME_ALLOWED_MSEC	(1 * 1000)
#define FLIP_EVENT_POLLING_TIMEOUT_MSEC		1000

__u8 facsimile_srm[] = {
	0x80, 0x0, 0x0, 0x05, 0x01, 0x0, 0x0, 0x36, 0x02, 0x51, 0x1E, 0xF2,
	0x1A, 0xCD, 0xE7, 0x26, 0x97, 0xF4, 0x01, 0x97, 0x10, 0x19, 0x92, 0x53,
	0xE9, 0xF0, 0x59, 0x95, 0xA3, 0x7A, 0x3B, 0xFE, 0xE0, 0x9C, 0x76, 0xDD,
	0x83, 0xAA, 0xC2, 0x5B, 0x24, 0xB3, 0x36, 0x84, 0x94, 0x75, 0x34, 0xDB,
	0x10, 0x9E, 0x3B, 0x23, 0x13, 0xD8, 0x7A, 0xC2, 0x30, 0x79, 0x84};

/**
 * List of Panels that should be excluded from hdcp tests
 *
 * This array is used to identify and handle scenarios where the test is
 * executed on dummy monitors, such as those found on shard machines.
 * Since these dummy monitors are not real and always the test is not consistent,
 * the test is skipped in such cases to avoid false negatives or
 * irrelevant test results.
 */
static const char *const hdcp_blocklist[] = {
	"DPF90435", /* Example monitor name */
	"SDC",
	/* Add more monitor names here as needed */
};

static void flip_handler(int fd, unsigned int sequence, unsigned int tv_sec,
			 unsigned int tv_usec, void *_data)
{
	igt_debug("Flip event received.\n");
}

static int wait_flip_event(void)
{
	int rc;
	drmEventContext evctx;
	struct pollfd pfd;

	evctx.version = 2;
	evctx.vblank_handler = NULL;
	evctx.page_flip_handler = flip_handler;

	pfd.fd = data.drm_fd;
	pfd.events = POLLIN;
	pfd.revents = 0;

	rc = poll(&pfd, 1, FLIP_EVENT_POLLING_TIMEOUT_MSEC);
	switch (rc) {
	case 0:
		igt_info("Poll timeout. 1Sec.\n");
		rc = -ETIMEDOUT;
		break;
	case 1:
		rc = drmHandleEvent(data.drm_fd, &evctx);
		igt_assert_eq(rc, 0);
		rc = 0;
		break;
	default:
		igt_info("Unexpected poll rc %d\n", rc);
		rc = -1;
		break;
	}

	return rc;
}

static bool
wait_for_prop_value(igt_output_t *output, uint64_t expected,
		    uint32_t timeout_mSec)
{
	uint64_t val;
	int i;

	if (data.cp_tests & CP_UEVENT && expected != CP_UNDESIRED) {
		igt_assert_f(igt_connector_event_detected(data.uevent_monitor,
							  output->id,
			     output->props[IGT_CONNECTOR_CONTENT_PROTECTION],
			     timeout_mSec / 1000), "uevent is not received");

		val = igt_output_get_prop(output,
					  IGT_CONNECTOR_CONTENT_PROTECTION);
		if (val == expected)
			return true;
	} else {
		for (i = 0; i < timeout_mSec; i++) {
			val = igt_output_get_prop(output,
						  IGT_CONNECTOR_CONTENT_PROTECTION);
			if (val == expected)
				return true;
			usleep(1000);
		}
	}

	igt_info("prop_value mismatch %" PRId64 " != %" PRId64 "\n",
		 val, expected);

	return false;
}

static void
commit_display_and_wait_for_flip(enum igt_commit_style commit_style)
{
	int ret;
	uint32_t flag;

	if (commit_style == COMMIT_ATOMIC) {
		flag = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_ALLOW_MODESET;
		igt_display_commit_atomic(&data.display, flag, NULL);

		ret = wait_flip_event();
		igt_assert_f(!ret, "wait_flip_event failed. %d\n", ret);
	} else {
		igt_display_commit2(&data.display, commit_style);

		/* Wait for 50mSec */
		usleep(50 * 1000);
	}
}

static void modeset_with_fb(const enum pipe pipe, igt_output_t *output,
			    enum igt_commit_style commit_style)
{
	igt_display_t *display = &data.display;
	drmModeModeInfo *mode;
	igt_plane_t *primary;

	mode = igt_output_get_mode(output);

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(primary, &data.red);
	igt_fb_set_size(&data.red, primary, mode->hdisplay, mode->vdisplay);

	igt_display_commit2(display, commit_style);

	igt_plane_set_fb(primary, &data.green);

	/* Wait for Flip completion before starting the HDCP authentication */
	commit_display_and_wait_for_flip(commit_style);
}

static bool test_cp_enable(igt_output_t *output, enum igt_commit_style commit_style,
			   int content_type, bool type_change)
{
	igt_display_t *display = &data.display;
	igt_plane_t *primary;
	bool ret;

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

	if (!type_change)
		igt_output_set_prop_value(output,
					  IGT_CONNECTOR_CONTENT_PROTECTION,
					  CP_DESIRED);

	if (output->props[IGT_CONNECTOR_HDCP_CONTENT_TYPE])
		igt_output_set_prop_value(output,
					  IGT_CONNECTOR_HDCP_CONTENT_TYPE,
					  content_type);
	igt_display_commit2(display, commit_style);

	ret = wait_for_prop_value(output, CP_ENABLED,
				  KERNEL_AUTH_TIME_ALLOWED_MSEC);
	if (ret) {
		igt_plane_set_fb(primary, &data.green);
		igt_display_commit2(display, commit_style);
	}

	return ret;
}

static void test_mst_cp_disable(igt_output_t *hdcp_mst_output[],
				enum igt_commit_style commit_style,
				int valid_outputs)
{
	igt_display_t *display = &data.display;
	igt_plane_t *primary;
	bool ret;
	int count;
	u64 val;

	for (count = 0; count < valid_outputs; count++) {
		primary = igt_output_get_plane_type(hdcp_mst_output[count], DRM_PLANE_TYPE_PRIMARY);
		igt_plane_set_fb(primary, &data.red);
		igt_output_set_prop_value(hdcp_mst_output[count], IGT_CONNECTOR_CONTENT_PROTECTION,
					  CP_UNDESIRED);
	}

	igt_display_commit2(display, commit_style);

	ret = wait_for_prop_value(hdcp_mst_output[0], CP_UNDESIRED,
				  KERNEL_DISABLE_TIME_ALLOWED_MSEC);
	for (count = 1; count < valid_outputs; count++) {
		val = igt_output_get_prop(hdcp_mst_output[count],
					  IGT_CONNECTOR_CONTENT_PROTECTION);
		ret &= (val == CP_UNDESIRED);
	}

	igt_assert_f(ret, "Content Protection not cleared on all MST outputs\n");
}

static void test_cp_disable(igt_output_t *output, enum igt_commit_style commit_style)
{
	igt_display_t *display = &data.display;
	igt_plane_t *primary;
	bool ret;

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

	/*
	 * Even on HDCP enable failed scenario, IGT should exit leaving the
	 * "content protection" at "UNDESIRED".
	 */
	igt_output_set_prop_value(output, IGT_CONNECTOR_CONTENT_PROTECTION,
				  CP_UNDESIRED);
	igt_plane_set_fb(primary, &data.red);
	igt_display_commit2(display, commit_style);

	/* Wait for HDCP to be disabled, before crtc off */
	ret = wait_for_prop_value(output, CP_UNDESIRED,
				  KERNEL_DISABLE_TIME_ALLOWED_MSEC);
	igt_assert_f(ret, "Content Protection not cleared\n");
}

static void test_cp_enable_with_retry(igt_output_t *output,
				      enum igt_commit_style commit_style,
				      int retry, int content_type,
				      bool expect_failure,
				      bool type_change)
{
	int retry_orig = retry;
	bool ret;

	do {
		if (!type_change || retry_orig != retry)
			test_cp_disable(output, commit_style);

		ret = test_cp_enable(output, commit_style, content_type, type_change);

		if (!ret && --retry)
			igt_debug("Retry (%d/2) ...\n", 3 - retry);
	} while (retry && !ret);

	if (!ret)
		test_cp_disable(output, commit_style);

	if (expect_failure)
		igt_assert_f(!ret,
			     "CP Enabled. Though it is expected to fail\n");
	else
		igt_assert_f(ret, "Content Protection not enabled\n");

}

static bool igt_pipe_is_free(igt_display_t *display, enum pipe pipe)
{
	int i;

	for (i = 0; i < display->n_outputs; i++)
		if (display->outputs[i].pending_pipe == pipe)
			return false;

	return true;
}

static void test_cp_lic(igt_output_t *output)
{
	bool ret;
	uint64_t val;

	/* Wait for 4Secs (min 2 cycles of Link Integrity Check) */
	ret = wait_for_prop_value(output, CP_DESIRED, LIC_PERIOD_MSEC);
	val = igt_output_get_prop(output,
				  IGT_CONNECTOR_CONTENT_PROTECTION);
	if (val == CP_DESIRED) {
		igt_debug("Link Integrity Check failed, waiting for reauthentication\n");
		ret = wait_for_prop_value(output, CP_DESIRED, LIC_PERIOD_MSEC);
	}

	igt_assert_f(!ret, "Content Protection LIC Failed\n");
}

static bool write_srm_as_fw(const __u8 *srm, int len)
{
	int fd, ret, total = 0;

	fd = open("/lib/firmware/display_hdcp_srm.bin",
		  O_WRONLY | O_CREAT, S_IRWXU);
	igt_require_f(fd >= 0, "Cannot write SRM binary to /lib/firmware\n");

	do {
		ret = write(fd, srm + total, len - total);
		if (ret < 0)
			ret = -errno;
		if (ret == -EINTR || ret == -EAGAIN)
			continue;
		if (ret <= 0)
			break;
		total += ret;
	} while (total != len);
	close(fd);

	return total < len ? false : true;
}

static void test_content_protection_on_output(igt_output_t *output,
					      enum pipe pipe,
					      enum igt_commit_style commit_style,
					      int content_type)
{
	igt_display_t *display = &data.display;
	bool ret;

	test_cp_enable_with_retry(output, commit_style, 3, content_type, false,
				  false);

	if (data.cp_tests & CP_TYPE_CHANGE) {
		/* Type 1 -> Type 0 */
		test_cp_enable_with_retry(output, commit_style, 3,
					  HDCP_CONTENT_TYPE_0, false,
					  true);
		/* Type 0 -> Type 1 */
		test_cp_enable_with_retry(output, commit_style, 3,
					  content_type, false,
					  true);
	}

	if (data.cp_tests & CP_MEI_RELOAD) {
		igt_assert_f(!igt_kmod_unload("mei_hdcp"),
			     "mei_hdcp unload failed");

		/* Expected to fail */
		test_cp_enable_with_retry(output, commit_style, 3,
					  content_type, true, false);

		igt_assert_f(!igt_kmod_load("mei_hdcp", NULL),
			     "mei_hdcp load failed");

		/* Expected to pass */
		test_cp_enable_with_retry(output, commit_style, 3,
					  content_type, false, false);
	}

	if (data.cp_tests & CP_LIC)
		test_cp_lic(output);

	if (data.cp_tests & CP_DPMS) {
		igt_pipe_set_prop_value(display, pipe,
					IGT_CRTC_ACTIVE, 0);
		igt_display_commit2(display, commit_style);

		igt_pipe_set_prop_value(display, pipe,
					IGT_CRTC_ACTIVE, 1);
		igt_display_commit2(display, commit_style);

		ret = wait_for_prop_value(output, CP_ENABLED,
					  KERNEL_AUTH_TIME_ALLOWED_MSEC);
		if (!ret)
			test_cp_enable_with_retry(output, commit_style, 2,
						  content_type, false,
						  false);
	}

	if (data.cp_tests & SUSPEND_RESUME) {
		igt_system_suspend_autoresume(SUSPEND_STATE_MEM, SUSPEND_TEST_NONE);

		ret = wait_for_prop_value(output, CP_ENABLED,
					  KERNEL_AUTH_TIME_ALLOWED_MSEC);
		if (!ret)
			test_cp_enable_with_retry(output, commit_style, 2,
						  content_type, false,
						  false);
	}
}

static void __debugfs_read(int fd, const char *param, char *buf, int len)
{
	len = igt_debugfs_simple_read(fd, param, buf, len);
	igt_require(len != -ENOENT);
	if (len < 0)
		igt_assert_eq(len, -ENODEV);
}

#define debugfs_read(fd, p, arr) __debugfs_read(fd, p, arr, sizeof(arr))

#define MAX_SINK_HDCP_CAP_BUF_LEN	5000

static bool sink_hdcp_capable(igt_output_t *output)
{
	char buf[MAX_SINK_HDCP_CAP_BUF_LEN];
	int fd;

	fd = igt_debugfs_connector_dir(data.drm_fd, output->name, O_RDONLY);
	if (fd < 0)
		return false;

	/*
	 * FIXME: As of now XE's debugfs is using i915 namespace. Once Kernel
	 * changes got landed, please update this logic to use XE specific
	 * debugfs.
	 */
	if (is_intel_device(data.drm_fd))
		debugfs_read(fd, "i915_hdcp_sink_capability", buf);
	else if (is_mtk_device(data.drm_fd))
		debugfs_read(fd, "mtk_hdcp_sink_capability", buf);
	else
		debugfs_read(fd, "hdcp_sink_capability", buf);

	close(fd);

	igt_debug("Sink capability: %s\n", buf);

	return strstr(buf, "HDCP1.4");
}

static bool sink_hdcp2_capable(igt_output_t *output)
{
	char buf[MAX_SINK_HDCP_CAP_BUF_LEN];
	int fd;

	fd = igt_debugfs_connector_dir(data.drm_fd, output->name, O_RDONLY);
	if (fd < 0)
		return false;

	/* FIXME: XE specific debugfs as mentioned above. */
	if (is_intel_device(data.drm_fd))
		debugfs_read(fd, "i915_hdcp_sink_capability", buf);
	else if (is_mtk_device(data.drm_fd))
		debugfs_read(fd, "mtk_hdcp_sink_capability", buf);
	else
		debugfs_read(fd, "hdcp_sink_capability", buf);

	close(fd);

	igt_debug("Sink capability: %s\n", buf);

	return strstr(buf, "HDCP2.2");
}

static void prepare_modeset_on_mst_output(igt_output_t *output, bool is_enabled)
{
	drmModeModeInfo *mode;
	igt_plane_t *primary;
	int width, height;

	mode = igt_output_get_mode(output);

	width = mode->hdisplay;
	height = mode->vdisplay;

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(primary, NULL);
	igt_plane_set_fb(primary, is_enabled ? &data.green : &data.red);
	igt_fb_set_size(is_enabled ? &data.green : &data.red, primary, width, height);
	igt_plane_set_size(primary, width, height);
}

static bool output_hdcp_capable(igt_output_t *output, int content_type)
{
		if (!output->props[IGT_CONNECTOR_CONTENT_PROTECTION])
			return false;

		if (!output->props[IGT_CONNECTOR_HDCP_CONTENT_TYPE] &&
		    content_type)
			return false;

		if (content_type && !sink_hdcp2_capable(output)) {
			igt_info("\tSkip %s (Sink has no HDCP2.2 support)\n",
				 output->name);
			return false;
		} else if (!sink_hdcp_capable(output)) {
			igt_info("\tSkip %s (Sink has no HDCP support)\n",
				 output->name);
			return false;
		}

		return true;
}

static void
test_fini(igt_output_t *output, enum igt_commit_style commit_style)
{
	igt_plane_t *primary;

	test_cp_disable(output, commit_style);
	primary = igt_output_get_plane_type(output,
					    DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(primary, NULL);
	igt_output_set_crtc(output, NULL);
	igt_display_commit2(&data.display, commit_style);
}

static bool is_output_hdcp_test_exempt(igt_output_t *output)
{
	drmModePropertyBlobPtr edid_blob = NULL;
	uint64_t edid_blob_id;
	const struct edid *edid;
	char edid_vendor[4];
	char sink_name[20];

	igt_assert(kmstest_get_property(data.drm_fd,
					output->config.connector->connector_id,
					DRM_MODE_OBJECT_CONNECTOR, "EDID", NULL,
					&edid_blob_id, NULL));

	igt_assert(edid_blob = drmModeGetPropertyBlob(data.drm_fd, edid_blob_id));

	edid = (const struct edid *)edid_blob->data;
	edid_get_mfg(edid, edid_vendor);
	edid_vendor[3] = '\0';

	edid_get_monitor_name(edid, sink_name, ARRAY_SIZE(sink_name));

	drmModeFreePropertyBlob(edid_blob);

	/* Not all monitors have sink names */
	if (sink_name[0] == '\0') {
		igt_debug("no sink name\n");
		return true;
	}

	return igt_is_panel_blocked(sink_name, hdcp_blocklist, ARRAY_SIZE(hdcp_blocklist));
}

static void
test_content_protection(enum igt_commit_style commit_style, int content_type)
{
	igt_display_t *display = &data.display;
	igt_output_t *output;
	enum pipe pipe;

	if (data.cp_tests & CP_MEI_RELOAD)
		igt_require_f(igt_kmod_is_loaded("mei_hdcp"),
			      "mei_hdcp module is not loaded\n");

	if (data.cp_tests & CP_UEVENT) {
		data.uevent_monitor = igt_watch_uevents();
		igt_flush_uevents(data.uevent_monitor);
	}

	for_each_connected_output(display, output) {
		for_each_pipe(display, pipe) {
			if (!output_hdcp_capable(output, content_type))
				continue;
			if (is_output_hdcp_test_exempt(output)) {
				igt_info("Skipping HDCP test on %s, as the panel is blocklisted\n",
					  output->name);
				continue;
			}

			igt_display_reset(display);
			igt_output_set_crtc(output,
				            igt_crtc_for_pipe(output->display, pipe));
			if (!intel_pipe_output_combo_valid(display))
				continue;

			modeset_with_fb(pipe, output, commit_style);

			igt_dynamic_f("pipe-%s-%s", kmstest_pipe_name(pipe), output->name)
				test_content_protection_on_output(output, pipe, commit_style, content_type);

			test_fini(output, commit_style);
			/*
			 * Testing a output with a pipe is enough for HDCP
			 * testing. No ROI in testing the connector with other
			 * pipes. So Break the loop on pipe.
			 */
			break;
		}
	}

	if (data.cp_tests & CP_UEVENT)
		igt_cleanup_uevents(data.uevent_monitor);
}

static bool output_is_dp_mst(igt_output_t *output, int i)
{
	int connector_id;
	static int prev_connector_id;

	connector_id = igt_get_dp_mst_connector_id(output);
	if (connector_id < 0)
		return false;

	/*
	 * Discarding outputs of other DP MST topology.
	 * Testing only on outputs on the topology we got previously
	 */
	if (i == 0) {
		prev_connector_id = connector_id;
	} else {
		if (connector_id != prev_connector_id)
			return false;
	}

	return true;
}

static void test_cp_lic_on_mst(igt_output_t *mst_outputs[], int valid_outputs, int first_output)
{
	int ret, count;
	uint64_t val;

	/* Only wait for the first output, this optimizes the test execution time */
	ret = wait_for_prop_value(mst_outputs[first_output], CP_DESIRED, LIC_PERIOD_MSEC);
	igt_assert_f(!ret, "Content Protection LIC Failed on %s\n",
		     mst_outputs[first_output]->name);

	for (count = first_output + 1; count < valid_outputs; count++) {
		val = igt_output_get_prop(mst_outputs[count], IGT_CONNECTOR_CONTENT_PROTECTION);
		igt_assert_f(val != CP_DESIRED, "Content Protection LIC Failed on %s\n", mst_outputs[count]->name);
	}
}

static void
test_mst_cp_enable_with_retry(igt_output_t *hdcp_mst_output[], int valid_outputs,
			      int retries, int content_type)
{
	igt_display_t *display = &data.display;
	int retry_orig = retries, count, i;
	bool ret;

	do {
		if (retry_orig != retries)
			test_mst_cp_disable(hdcp_mst_output, COMMIT_ATOMIC, valid_outputs);

		for (count = 0; count < valid_outputs; count++) {
			igt_output_set_prop_value(hdcp_mst_output[count],
						  IGT_CONNECTOR_CONTENT_PROTECTION, CP_DESIRED);

			if (hdcp_mst_output[count]->props[IGT_CONNECTOR_HDCP_CONTENT_TYPE])
				igt_output_set_prop_value(hdcp_mst_output[count],
							  IGT_CONNECTOR_HDCP_CONTENT_TYPE,
							  content_type);
		}

		igt_display_commit2(display, COMMIT_ATOMIC);

		ret = wait_for_prop_value(hdcp_mst_output[0], CP_ENABLED,
					  KERNEL_AUTH_TIME_ALLOWED_MSEC);
		for (count = 1; count < valid_outputs; count++)
			ret &= wait_for_prop_value(hdcp_mst_output[count], CP_ENABLED,
						  KERNEL_AUTH_TIME_ADDITIONAL_MSEC);

		retries -= 1;

		if (!ret || retries)
			igt_debug("Retry %d/3\n", 3 - retries);

		for (i = 0; i < valid_outputs; i++)
			prepare_modeset_on_mst_output(hdcp_mst_output[i], ret);

		igt_display_commit2(display, COMMIT_ATOMIC);
	} while (retries && !ret);

	igt_assert_f(ret, "Content Protection not enabled on MST outputs\n");
}

static void
test_content_protection_mst(int content_type)
{
	igt_display_t *display = &data.display;
	igt_output_t *output;
	int valid_outputs = 0, dp_mst_outputs = 0, ret, count, max_pipe = 0, i;
	enum pipe pipe;
	bool pipe_found;
	igt_output_t *hdcp_mst_output[IGT_MAX_PIPES];

	for_each_pipe(display, pipe)
		max_pipe++;

	pipe = PIPE_A;

	for_each_connected_output(display, output) {
		if (!output_is_dp_mst(output, dp_mst_outputs))
			continue;

		pipe_found = false;
		for_each_pipe(display, pipe) {
			if (igt_pipe_is_free(display, pipe) &&
			    igt_pipe_connector_valid(pipe, output)) {
				pipe_found = true;
				break;
			}
		}

		igt_assert_f(pipe_found, "No valid pipe found for %s\n", output->name);

		igt_output_set_crtc(output,
				    igt_crtc_for_pipe(output->display, pipe));
		prepare_modeset_on_mst_output(output, false);
		dp_mst_outputs++;
		if (output_hdcp_capable(output, content_type))
			hdcp_mst_output[valid_outputs++] = output;
	}

	igt_require_f(dp_mst_outputs > 1, "No DP MST set up with >= 2 outputs found in a single topology\n");
	igt_require_f(valid_outputs > 1, "DP MST outputs do not have the required HDCP support\n");

	if (igt_display_try_commit_atomic(display,
				DRM_MODE_ATOMIC_TEST_ONLY |
				DRM_MODE_ATOMIC_ALLOW_MODESET,
				NULL) != 0) {
		bool found = igt_override_all_active_output_modes_to_fit_bw(display);
		igt_require_f(found, "No valid mode combo found for MST modeset\n");

		for (count = 0; count < valid_outputs; count++)
			prepare_modeset_on_mst_output(hdcp_mst_output[count], false);

		ret = igt_display_try_commit2(display, COMMIT_ATOMIC);
		igt_require_f(ret == 0, "Commit failure during MST modeset\n");
	}

	igt_display_commit2(display, COMMIT_ATOMIC);

	test_mst_cp_enable_with_retry(hdcp_mst_output, valid_outputs, 2, content_type);

	if (data.cp_tests & SUSPEND_RESUME) {
		igt_system_suspend_autoresume(SUSPEND_STATE_MEM, SUSPEND_TEST_NONE);

		ret = wait_for_prop_value(hdcp_mst_output[0],
						 CP_ENABLED,
						 KERNEL_AUTH_TIME_ALLOWED_MSEC);
		if (!ret)
			test_mst_cp_enable_with_retry(hdcp_mst_output, valid_outputs,
						      2, content_type);
	}

	if (data.cp_tests & CP_LIC)
		test_cp_lic_on_mst(hdcp_mst_output, valid_outputs, 0);

	/*
	 * Verify if CP is still enabled on other outputs by disabling CP on the first output.
	 */
	igt_debug("CP Prop being UNDESIRED on %s\n", hdcp_mst_output[0]->name);
	test_cp_disable(hdcp_mst_output[0], COMMIT_ATOMIC);

	/* CP is expected to be still enabled on other outputs*/
	for (i = 1; i < valid_outputs; i++) {
		/* Wait for the timeout to verify CP is not disabled */
		ret = wait_for_prop_value(hdcp_mst_output[i], CP_UNDESIRED, KERNEL_DISABLE_TIME_ALLOWED_MSEC);
		igt_assert_f(!ret, "Content Protection not enabled on %s\n", hdcp_mst_output[i]->name);
	}

	if (data.cp_tests & CP_LIC)
		test_cp_lic_on_mst(hdcp_mst_output, valid_outputs, 1);
}


static void test_content_protection_cleanup(void)
{
	igt_display_t *display = &data.display;
	igt_output_t *output;
	uint64_t val;

	for_each_connected_output(display, output) {
		if (!output->props[IGT_CONNECTOR_CONTENT_PROTECTION])
			continue;

		val = igt_output_get_prop(output,
					  IGT_CONNECTOR_CONTENT_PROTECTION);
		if (val == CP_UNDESIRED)
			continue;

		igt_info("CP Prop being UNDESIRED on %s\n", output->name);
		test_cp_disable(output, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);
	}

	igt_remove_fb(data.drm_fd, &data.red);
	igt_remove_fb(data.drm_fd, &data.green);
}

static void create_fbs(void)
{
	uint16_t width = 0, height = 0;
	drmModeModeInfo *mode;
	igt_output_t *output;

	for_each_connected_output(&data.display, output) {
		mode = igt_output_get_mode(output);
		igt_assert(mode);

		width = max(width, mode->hdisplay);
		height = max(height, mode->vdisplay);
	}

	igt_create_color_fb(data.drm_fd, width, height,
			    DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
			    1.f, 0.f, 0.f, &data.red);
	igt_create_color_fb(data.drm_fd, width, height,
			    DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
			    0.f, 1.f, 0.f, &data.green);
}

static const struct {
	const char *desc;
	const char *name;
	unsigned int cp_tests;
	bool content_type;
} subtests[] = {
	{ .desc = "Test content protection with atomic modesetting",
	  .name = "atomic",
	  .cp_tests = 0,
	  .content_type = HDCP_CONTENT_TYPE_0
	},
	{ .desc = "Test content protection with DPMS ON/OFF during atomic modesetting.",
	  .name = "atomic-dpms",
	  .cp_tests = CP_DPMS,
	  .content_type = HDCP_CONTENT_TYPE_0
	},
	{ .desc = "Test for the integrity of link with type 0 content.",
	  .name = "lic-type-0",
	  .cp_tests = CP_LIC,
	  .content_type = HDCP_CONTENT_TYPE_0,
	},
	{ .desc = "Test for the integrity of link with type 1 content",
	  .name = "lic-type-1",
	  .cp_tests = CP_LIC,
	  .content_type = HDCP_CONTENT_TYPE_1,
	},
	{ .desc = "Test content protection with content type 1 "
		  "that can be handled only through HDCP2.2.",
	  .name = "type1",
	  .cp_tests = 0,
	  .content_type = HDCP_CONTENT_TYPE_1,
	},
	{ .desc = "Test the teardown and rebuild of the interface between "
		  "Intel and mei hdcp.",
	  .name = "mei-interface",
	  .cp_tests = CP_MEI_RELOAD,
	  .content_type = HDCP_CONTENT_TYPE_1,
	},
	{ .desc = "Test the content type change when the content protection already enabled",
	  .name = "content-type-change",
	  .cp_tests = CP_TYPE_CHANGE,
	  .content_type = HDCP_CONTENT_TYPE_1,
	},
	{ .desc = "Test to detect the HDCP status change when we are reading the uevent "
		  "sent with the corresponding connector id and property id.",
	  .name = "uevent",
	  .cp_tests = CP_UEVENT,
	  .content_type = HDCP_CONTENT_TYPE_0,
	},
	/*
	 *  Testing the revocation check through SRM needs a HDCP sink with
	 *  programmable Ksvs or we need a uAPI from kernel to read the
	 *  connected HDCP sink's Ksv. With that we would be able to add that
	 *  Ksv into a SRM and send in for revocation check. Since we dont have
	 *  either of these options, we test SRM writing from userspace and
	 *  validation of the same at kernel. Something is better than nothing.
	 */
	{ .desc = "This test writes the facsimile SRM into the /lib/firmware/ "
		  "and check the kernel parsing of it by invoking the hdcp authentication.",
	  .name = "srm",
	  .cp_tests = 0,
	  .content_type = HDCP_CONTENT_TYPE_0,
	},
	{.desc = "Test to verify the behaviour of HDCP after suspend resume cycles.",
	 .name = "suspend-resume",
	 .cp_tests = SUSPEND_RESUME,
	 .content_type = HDCP_CONTENT_TYPE_0,
	}
};

static const struct {
	const char *desc;
	const char *name;
	unsigned int cp_tests;
	bool content_type;
} mst_subtests[] = {
	{ .desc = "Test Content protection(Type 0) over DP MST.",
	  .name = "dp-mst-type-0",
	  .cp_tests = 0,
	  .content_type = HDCP_CONTENT_TYPE_0
	},
	{ .desc = "Test Content protection(Type 0) over DP MST with LIC.",
	  .name = "dp-mst-lic-type-0",
	  .cp_tests = CP_LIC,
	  .content_type = HDCP_CONTENT_TYPE_0
	},
	{ .desc = "Test Content protection(Type 1) over DP MST.",
	  .name = "dp-mst-type-1",
	  .cp_tests = 0,
	  .content_type = HDCP_CONTENT_TYPE_1,
	},
	{ .desc = "Test Content protection(Type 1) over DP MST with LIC.",
	  .name = "dp-mst-lic-type-1",
	  .cp_tests = CP_LIC,
	  .content_type = HDCP_CONTENT_TYPE_1,
	},
	{ .desc = "Test Content protection(Type 1) over DP MST with suspend resume.",
	  .name = "dp-mst-suspend-resume",
	  .cp_tests = SUSPEND_RESUME,
	  .content_type = HDCP_CONTENT_TYPE_1,
	},
};

int igt_main()
{
	igt_fixture() {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);
		igt_display_require(&data.display, data.drm_fd);
		igt_display_require_output(&data.display);
		create_fbs();
	}

	igt_describe("Test content protection with legacy style commit.");
	igt_subtest_with_dynamic("legacy") {
		data.cp_tests = 0;
		test_content_protection(COMMIT_LEGACY, HDCP_CONTENT_TYPE_0);
	}

	igt_subtest_group() {
		igt_fixture()
			igt_require(data.display.is_atomic);

		for (int i = 0; i < ARRAY_SIZE(subtests); i++) {
			igt_describe_f("%s", subtests[i].desc);

			igt_subtest_with_dynamic(subtests[i].name) {
				data.cp_tests = subtests[i].cp_tests;

				if (!strcmp(subtests[i].name, "srm")) {
					bool ret;

					ret = write_srm_as_fw((const __u8 *)facsimile_srm,
							     sizeof(facsimile_srm));
					igt_assert_f(ret, "SRM update failed");
				}

				test_content_protection(COMMIT_ATOMIC, subtests[i].content_type);
			}
		}
	}

	igt_subtest_group() {
		igt_fixture()
			igt_require(data.display.is_atomic);

		for (int i = 0; i < ARRAY_SIZE(mst_subtests); i++) {
			igt_describe_f("%s", mst_subtests[i].desc);

			igt_subtest(mst_subtests[i].name) {
				data.cp_tests = mst_subtests[i].cp_tests;
				test_content_protection_mst(mst_subtests[i].content_type);
			}
		}
	}

	igt_fixture() {
		test_content_protection_cleanup();
		igt_display_fini(&data.display);
		drm_close_driver(data.drm_fd);
	}
}
