// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

/**
 * TEST: Basic tests for GuC based register capture
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: CMD submission
 * Functionality: Debug
 * Test category: functionality test
 */

#include <ctype.h>
#include <fcntl.h>
#include <inttypes.h>
#include <regex.h>
#include <stdio.h>
#include <string.h>

#include "igt.h"
#include "igt_device.h"
#include "igt_sriov_device.h"
#include "igt_sysfs.h"
#include "lib/igt_syncobj.h"
#include "lib/intel_reg.h"
#include "linux_scaffold.h"
#include "xe_drm.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_legacy.h"
#include "xe/xe_query.h"
#include "xe/xe_spin.h"

#define MAX_N_EXECQUEUES		16
#define CAPTURE_JOB_TIMEOUT		2000
#define JOB_TIMOUT_ENTRY		"job_timeout_ms"

#define BASE_ADDRESS			0x1a0000
#define ADDRESS_SHIFT			39
#define CID_ADDRESS_MASK		0x7F
/* Batch buffer element count, in number of dwords(u32) */
#define BATCH_DW_COUNT			16

#define MAX_TEMP_LEN			80
#define MAX_SYSFS_PATH_LEN		128
#define MAX_LINES			4096
/* Max line buffer size (includes last '\0') */
#define MAX_LINE_LEN			1024
#define MAIN_BUF_SIZE			(MAX_LINES * MAX_LINE_LEN * sizeof(char))
/*
 * Devcoredump might have long line this test don't care.
 * This buffer size used when load dump content
 */
#define LINE_BUF_SIZE			(64 * 1024)

#define DUMP_PATH			"/sys/class/drm/card%d/device/devcoredump/data"
#define START_TAG			"**** Job ****"

/* Optional Space */
#define SPC_O				"[ \t\\.]*"
/* Required Space */
#define SPC				"[ \t\\.]+"
/* Optional Non-Space */
#define NSPC_O				"([^ \t\\.]*)"
/* Required Non-Space */
#define NSPC				"([^ \t\\.]+)"
#define BEG				"^" SPC_O
#define REQ_FIELD			NSPC SPC
#define REQ_FIELD_LAST			NSPC SPC_O
#define OPT_FIELD			NSPC_O SPC_O
#define END				SPC_O "$"

#define REGEX_NON_SPACE_GROUPS	BEG REQ_FIELD REQ_FIELD_LAST OPT_FIELD OPT_FIELD OPT_FIELD END
#define REGEX_NON_SPACE_GROUPS_COUNT	6

#define INDEX_KEY			1
#define INDEX_VALUE			2
#define INDEX_ENGINE_PHYSICAL		2
#define INDEX_ENGINE_NAME		1
#define INDEX_ENGINE_INSTANCE		4
#define INDEX_VM_LENGTH			2
#define INDEX_VM_SIZE			3

static u64
xe_sysfs_get_job_timeout_ms(int fd, struct drm_xe_engine_class_instance *eci)
{
	int engine_fd = -1;
	u64 ret;

	engine_fd = xe_sysfs_engine_open(fd, eci->gt_id, eci->engine_class);
	ret = igt_sysfs_get_u64(engine_fd, JOB_TIMOUT_ENTRY);
	close(engine_fd);

	return ret;
}

static void xe_sysfs_set_job_timeout_ms(int fd, struct drm_xe_engine_class_instance *eci,
					u64 timeout)
{
	int engine_fd = -1;

	engine_fd = xe_sysfs_engine_open(fd, eci->gt_id, eci->engine_class);
	igt_sysfs_set_u64(engine_fd, JOB_TIMOUT_ENTRY, timeout);
	close(engine_fd);
}

static char *safe_strncpy(char *dst, const char *src, int n)
{
	char *s;

	igt_assert(n > 0);
	igt_assert(dst && src);

	s = strncpy(dst, src, n - 1);
	s[n - 1] = '\0';

	return s;
}

static const char *xe_engine_class_name(u32 engine_class)
{
	switch (engine_class) {
	case DRM_XE_ENGINE_CLASS_RENDER:
		return "rcs";
	case DRM_XE_ENGINE_CLASS_COPY:
		return "bcs";
	case DRM_XE_ENGINE_CLASS_VIDEO_DECODE:
		return "vcs";
	case DRM_XE_ENGINE_CLASS_VIDEO_ENHANCE:
		return "vecs";
	case DRM_XE_ENGINE_CLASS_COMPUTE:
		return "ccs";
	default:
		igt_warn("Engine class 0x%x unknown\n", engine_class);
		return "unknown";
	}
}

static char **alloc_lines_buffer(void)
{
	int i;
	char **lines = (char **)malloc(MAX_LINES * sizeof(char *));
	char *main_buf =  (char *)malloc(MAIN_BUF_SIZE);

	igt_assert_f(lines, "Out of memory.\n");
	igt_assert_f(main_buf, "Out of memory.\n");

	/* set string array pointers */
	for (i = 0; i < MAX_LINES; i++)
		lines[i] = main_buf + i * MAX_LINE_LEN;

	return lines;
}

static char *get_devcoredump_path(int card_id, char *buf)
{
	sprintf(buf, DUMP_PATH, card_id);
	return buf;
}

static int load_all(FILE *fd, char **lines, char *buf)
{
	int start_line = 0, i = 0;
	bool skip = true;

	memset(lines[0], 0, MAIN_BUF_SIZE);
	while (!feof(fd) && i < MAX_LINES) {
		/*
		 * Devcoredump might have long lines, load up to
		 * LINE_BUF_SIZE for a single line
		 */
		if (!fgets(buf, LINE_BUF_SIZE, fd))
			if (ferror(fd) != 0) {
				igt_warn("Failed to read devcoredump file, error: %d\n",
					 ferror(fd));
				break;
			}

		if (skip) {
			start_line++;
			/* Skip all lines before START_TAG */
			if (strncmp(START_TAG, buf, strlen(START_TAG)))
				continue;
			else
				skip = false;
		}

		/* Only save up to MAX_LINE_LEN to buffer */
		safe_strncpy(lines[i++], buf, MAX_LINE_LEN);
	}
	return start_line;
}

static int access_devcoredump(char *path, char **lines, char *line_buf)
{
	int start_line = -1;
	FILE *fd = fopen(path, "r");

	if (!fd)
		return false;

	igt_debug("Devcoredump found: %s\n", path);

	/* Clear memory before load file */
	if (lines)
		start_line = load_all(fd, lines, line_buf);

	fclose(fd);
	return start_line;
}

static bool rm_devcoredump(char *path)
{
	int fd = open(path, O_WRONLY);

	if (fd != -1) {
		igt_debug("Clearing devcoredump.\n");
		write(fd, "0", 1);
		close(fd);
		return true;
	}

	return false;
}

static char
*get_coredump_item(regex_t *regex, char **lines, const char *tag, int tag_index, int target_index)
{
	int i;
	regmatch_t match[REGEX_NON_SPACE_GROUPS_COUNT];

	for (i = 0; i < MAX_LINES; i++) {
		char *line = lines[i];

		/* Skip lines without tag */
		if (!strstr(line, tag))
			continue;

		if ((regexec(regex, line, REGEX_NON_SPACE_GROUPS_COUNT, match, 0)) == 0) {
			char *key = NULL, *value = NULL;

			if (match[tag_index].rm_so >= 0) {
				key = &line[match[tag_index].rm_so];
				line[match[tag_index].rm_eo] = '\0';
			}
			if (match[target_index].rm_so >= 0) {
				value = &line[match[target_index].rm_so];
				line[match[target_index].rm_eo] = '\0';
			}
			if (key && value && strcmp(tag, key) == 0)
				return value;
			/* if key != tag,  keep searching and loop to next line */
		}
	}

	return NULL;
}

static uint64_t
compare_hex_value(const char *output)
{
	char result[64];
	uint64_t ret_val;
	char *src = (char *)output, *dst = result;

//example i/p : [1580001a0000].length: 0x10000
	while (*src) {
		if (*src == '[' || *src == ']') {
			src++;
			continue;
		}

		*dst = toupper((unsigned char)*src);
		dst++;
		src++;
	}
	*dst = '\0';
	ret_val = strtoull(result, NULL, 16);
	return ret_val;
}

static void
check_item_u64(regex_t *regex, char **lines, const char *tag, u64 addr_lo,
	       u64 addr_hi, int tag_index, int target_index)
{
	u64 result;
	char *output;

	igt_assert_f((output = get_coredump_item(regex, lines, tag, tag_index, target_index)),
		     "Target not found:%s\n", tag);

	result = compare_hex_value(output);
	igt_debug("Compare %s %s vs [0x%" PRIX64 "-0x%" PRIX64 "] result %" PRIX64 "\n", tag, output,
		  addr_lo, addr_hi, result);
	igt_assert_f((addr_lo <= result) && (result <= addr_hi),
		     "value %" PRIX64 " out of range[0x%" PRIX64 "-0x%" PRIX64 "]\n", result, addr_lo, addr_hi);
}

static void
check_item_str(regex_t *regex, char **lines, const char *tag, int tag_index, int target_index,
	       const char *target, bool up_to_target_len)
{
	char buf[MAX_TEMP_LEN] = {0};
	char *output;
	int code;

	igt_assert_f(output = get_coredump_item(regex, lines, tag, tag_index, target_index),
		     "Target not found:%s\n", tag);

	if (up_to_target_len) {
		igt_assert_f(strlen(target) < MAX_TEMP_LEN, "Target too long.\n");
		safe_strncpy(buf, output, MAX_TEMP_LEN);
		buf[strlen(target)] = 0;
		output = buf;
	}
	code = strncmp(output, target, strlen(target));
	igt_debug("From tag '%s' found %s vs %s\n", tag, output, target);
	igt_assert_f(code == 0, "Expected value:%s, received:%s\n", target, output);
}

/**
 * SUBTEST: reset
 * Description: Reset GuC, check devcoredump output values
 */
static void test_card(int fd)
{
	struct drm_xe_engine_class_instance *hwe;
	regex_t regex;
	int start_line;
	int engine_cid = rand();
	char **lines;
	char *single_line_buf =  (char *)malloc(LINE_BUF_SIZE);
	char temp[MAX_TEMP_LEN];
	char path[MAX_SYSFS_PATH_LEN];
	const bool is_vf_device = intel_is_vf_device(fd);

	igt_assert_f(single_line_buf, "Out of memory.\n");

	regcomp(&regex, REGEX_NON_SPACE_GROUPS, REG_EXTENDED | REG_NEWLINE);
	get_devcoredump_path(igt_device_get_card_index(fd), path);
	lines = alloc_lines_buffer();

	/* clear old devcoredump, if any */
	rm_devcoredump(path);

	xe_for_each_engine(fd, hwe) {
		/*
		 * To test devcoredump register data, the test batch address is
		 * used to compare with the dump, address bit 40 to 46 act as
		 * context id, which start with an random number, increased 1
		 * per engine. By this way, the address is unique for each
		 * engine, and start with an random number on each run.
		 */
		const u64 addr = BASE_ADDRESS | ((u64)(engine_cid++ % CID_ADDRESS_MASK) <<
						 ADDRESS_SHIFT);

		igt_debug("Running on engine class: %x instance: %x\n", hwe->engine_class,
			  hwe->engine_instance);

		xe_legacy_test_mode(fd, hwe, 1, 1, DRM_XE_VM_BIND_FLAG_DUMPABLE,
				    addr, true);

		/* Wait 1 sec for devcoredump complete */
		sleep(1);

		/* assert devcoredump created */
		igt_assert_f((start_line = access_devcoredump(path, lines, single_line_buf)) > 0,
			     "Devcoredump not exist, errno=%d.\n", errno);

		if (!is_vf_device) {
			sprintf(temp, "instance=%d", hwe->engine_instance);
			check_item_str(&regex, lines, "(physical),",
				       INDEX_ENGINE_PHYSICAL,
				       INDEX_ENGINE_INSTANCE, temp, false);
			check_item_str(&regex, lines, "(physical),",
				       INDEX_ENGINE_PHYSICAL, INDEX_ENGINE_NAME,
				       xe_engine_class_name(hwe->engine_class),
				       true);

			check_item_str(&regex, lines,
				       "Capture_source:", INDEX_KEY,
				       INDEX_VALUE, "GuC", false);

			check_item_u64(&regex, lines, "ACTHD:", addr,
				       addr + BATCH_DW_COUNT * sizeof(u32),
				       INDEX_KEY, INDEX_VALUE);
			check_item_u64(&regex, lines, "RING_BBADDR:", addr,
				       addr + BATCH_DW_COUNT * sizeof(u32),
				       INDEX_KEY, INDEX_VALUE);
		}
		check_item_u64(&regex, lines, "length:", addr,
			       addr + BATCH_DW_COUNT * sizeof(u32), INDEX_VALUE, INDEX_KEY);

		/* clear devcoredump */
		rm_devcoredump(path);
		sleep(1);
		/* Assert devcoredump removed */
		igt_assert_f(!access_devcoredump(path, NULL, NULL), "Devcoredump not removed\n");
	}
	/* Free lines buffer */
	free(lines);
	free(single_line_buf);
	regfree(&regex);
}

int igt_main()
{
	int xe;
	struct drm_xe_engine_class_instance *hwe;
	u64 timeouts[DRM_XE_ENGINE_CLASS_VM_BIND] = {0};

	igt_fixture() {
		xe = drm_open_driver(DRIVER_XE);
		xe_for_each_engine(xe, hwe) {
			/* Skip kernel only classes */
			if (hwe->engine_class >= DRM_XE_ENGINE_CLASS_VM_BIND)
				continue;
			/* Skip classes already set */
			if (timeouts[hwe->engine_class])
				continue;
			/* Save original timeout value */
			timeouts[hwe->engine_class] = xe_sysfs_get_job_timeout_ms(xe, hwe);
			/* Reduce timeout value to speedup test */
			xe_sysfs_set_job_timeout_ms(xe, hwe, CAPTURE_JOB_TIMEOUT);

			igt_debug("Reduced %s class timeout from %" PRIu64 " to %d\n",
				  xe_engine_class_name(hwe->engine_class),
				  timeouts[hwe->engine_class], CAPTURE_JOB_TIMEOUT);
		}
	}

	igt_subtest("reset")
		test_card(xe);

	igt_fixture() {
		xe_for_each_engine(xe, hwe) {
			u64 store, timeout;

			/* Skip kernel only classes */
			if (hwe->engine_class >= DRM_XE_ENGINE_CLASS_VM_BIND)
				continue;

			timeout = timeouts[hwe->engine_class];
			/* Skip classes already set */
			if (!timeout)
				continue;

			/* Restore original timeout value */
			xe_sysfs_set_job_timeout_ms(xe, hwe, timeout);

			/* Assert successful restore */
			store = xe_sysfs_get_job_timeout_ms(xe, hwe);
			igt_abort_on_f(timeout != store, "job_timeout_ms not restored!\n");

			igt_debug("Restored %s class timeout to %" PRIu64 "\n",
				  xe_engine_class_name(hwe->engine_class),
				  timeouts[hwe->engine_class]);

			timeouts[hwe->engine_class] = 0;
		}

		drm_close_driver(xe);
	}
}
