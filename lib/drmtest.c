/*
 * Copyright © 2007, 2011, 2013 Intel Corporation
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
 *    Eric Anholt <eric@anholt.net>
 *    Daniel Vetter <daniel.vetter@ffwll.ch>
 *
 */

#ifdef HAVE_LIBGEN_H
#include <libgen.h>
#endif
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/mman.h>
#include <signal.h>
#include <pciaccess.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <termios.h>
#include <pthread.h>

#include "drmtest.h"
#include "i915_drm.h"
#include "i915/gem.h"
#include "xe/xe_query.h"
#include "intel_chipset.h"
#include "intel_io.h"
#include "igt_debugfs.h"
#include "igt_device.h"
#include "igt_gt.h"
#include "igt_kmod.h"
#include "igt_params.h"
#include "igt_sysfs.h"
#include "igt_device_scan.h"
#include "version.h"
#include "config.h"
#include "intel_reg.h"
#include "ioctl_wrappers.h"
#include "igt_dummyload.h"
#include "xe/xe_query.h"

/**
 * SECTION:drmtest
 * @short_description: Base library for drm tests and tools
 * @title: drmtest
 * @include: igt.h
 *
 * This library contains the basic support for writing tests, with the most
 * important part being the helper function to open drm device nodes.
 *
 * But there's also a bit of other assorted stuff here.
 *
 * Note that this library's header pulls in the [i-g-t core](igt-gpu-tools-i-g-t-core.html)
 * and [batchbuffer](igt-gpu-tools-intel-batchbuffer.html) libraries as dependencies.
 */

/**
 * __get_drm_device_name:
 * @fd: a drm file descriptor
 * @name: pointer to memory
 * @name_size: size of @name
 *
 * A wrapper for DRM_IOCTL_VERSION which will write drm device name in @name.
 *
 * Returns:
 * 0  if name of DRM driver was filled in @name
 * -1 on ioctl fail
 */
int __get_drm_device_name(int fd, char *name, int name_size)
{
	drm_version_t version;

	memset(&version, 0, sizeof(version));
	version.name_len = name_size;
	version.name = name;

	if (!drmIoctl(fd, DRM_IOCTL_VERSION, &version)){
		return 0;
	}

	return -1;
}

static bool __is_device(int fd, const char *expect)
{
	char name[12] = "";

	if (__get_drm_device_name(fd, name, sizeof(name) - 1))
		return false;

	return strcmp(expect, name) == 0;
}

bool is_vkms_device(int fd)
{
	return __is_device(fd, "vkms");
}

bool is_amdgpu_device(int fd)
{
	return __is_device(fd, "amdgpu");
}

bool is_i915_device(int fd)
{
	return __is_device(fd, "i915");
}

bool is_mtk_device(int fd)
{
	return __is_device(fd, "mediatek");
}

bool is_msm_device(int fd)
{
	return __is_device(fd, "msm");
}

bool is_nouveau_device(int fd)
{
	/* Currently all nouveau-specific codepaths require libdrm */
#ifdef HAVE_LIBDRM_NOUVEAU
	return __is_device(fd, "nouveau");
#else
	return false;
#endif
}

bool is_vc4_device(int fd)
{
	return __is_device(fd, "vc4");
}

bool is_xe_device(int fd)
{
	return __is_device(fd, "xe");
}

bool is_intel_device(int fd)
{
	return is_i915_device(fd) || is_xe_device(fd);
}

enum intel_driver get_intel_driver(int fd)
{
	if (is_xe_device(fd))
		return INTEL_DRIVER_XE;
	else if (is_i915_device(fd))
		return INTEL_DRIVER_I915;

	igt_assert_f(0, "Device is not handled by Intel driver\n");
}

static char _forced_driver[16] = "";

/**
 * __set_forced_driver:
 * @name: name of driver to forcibly use
 *
 * Set the name of a driver to use when calling #drm_open_driver with
 * the #DRIVER_ANY flag.
 */
void __set_forced_driver(const char *name)
{
	if (!name) {
		igt_warn("No driver specified, keep default behaviour\n");
		return;
	}

	strncpy(_forced_driver, name, sizeof(_forced_driver) - 1);
}

static const char *forced_driver(void)
{
	if (_forced_driver[0])
		return _forced_driver;

	return NULL;
}

static int modprobe(const char *driver)
{
	return igt_kmod_load(driver, "");
}

static void modprobe_i915(const char *name)
{
	/* When loading i915, we also want to load snd-hda et al */
	igt_i915_driver_load(NULL);
}

static const struct module {
	unsigned int bit;
	const char *module;
	void (*modprobe)(const char *name);
} modules[] = {
	{ DRIVER_AMDGPU, "amdgpu" },
	{ DRIVER_INTEL, "i915", modprobe_i915 },
	{ DRIVER_MSM, "msm" },
	{ DRIVER_PANFROST, "panfrost" },
	{ DRIVER_PANTHOR, "panthor" },
	{ DRIVER_V3D, "v3d" },
	{ DRIVER_VC4, "vc4" },
	{ DRIVER_VGEM, "vgem" },
	{ DRIVER_VIRTIO, "virtio_gpu" },
	{ DRIVER_VKMS, "vkms" },
	{ DRIVER_VMWGFX, "vmwgfx" },
	{ DRIVER_XE, "xe" },
	{}
};

struct _opened_device_path {
	char *path;
	struct igt_list_head link;
};

static void modulename_to_chipset(const char *name, unsigned int *chip)
{
	if (!name)
		return;

	for (int start = 0, end = ARRAY_SIZE(modules) - 1; start < end; ) {
		int mid = start + (end - start) / 2;
		int ret = strcmp(modules[mid].module, name);

		if (ret < 0) {
			start = mid + 1;
		} else if (ret > 0) {
			end = mid;
		} else {
			*chip = modules[mid].bit;
			break;
		}
	}
}

/**
 * drm_get_chipset:
 * @fd: a drm file descriptor
 *
 * Returns:
 * chipset if driver name found in modules[] array, for example: DRIVER_INTEL
 * DRIVER_ANY if drm device name not known
 */
unsigned int drm_get_chipset(int fd)
{
	unsigned int chip = DRIVER_ANY;
	char name[32] = "";

	if (__get_drm_device_name(fd, name, sizeof(name) - 1))
		return chip;

	modulename_to_chipset(name, &chip);

	return chip;
}

static const char *chipset_to_str(int chipset)
{
	if (chipset == DRIVER_INTEL)
		return "intel";

	for (int i = 0, end = ARRAY_SIZE(modules); i < end; i++)
		if (modules[i].bit == chipset)
			return modules[i].module;

	return chipset == DRIVER_ANY ? "any" : "other";
}


/*
 * Logs path of opened device. Device path opened for the first time is logged at info level,
 * subsequent opens (if any) are logged at debug level.
 */
static void log_opened_device_path(const char *device_path)
{
	static IGT_LIST_HEAD(opened_paths);
	struct _opened_device_path *item;

	igt_list_for_each_entry(item, &opened_paths, link) {
		if (!strcmp(item->path, device_path)) {
			igt_debug("Opened previously opened device: %s\n", device_path);
			return;
		}
	}

	item = calloc(1, sizeof(struct _opened_device_path));
	igt_assert(item);
	item->path = strdup(device_path);
	igt_assert(item->path);
	igt_list_add(&item->link, &opened_paths);
	igt_info("Opened device: %s\n", item->path);
}

/**
 * __drm_open_device:
 * @name: DRM node name
 * @chipset: OR'd flags for chipset to be opened
 *
 * Open a drm legacy device node with given @name and compatible with given
 * @chipset flag.
 *
 * A special case is the use of the IGT_FORCE_DRIVER environment variable. In
 * such case, even if opened device is compatible with given @chipset flag, the
 * function returns error if forced driver is not compatible with @chipset.
 * In case of DRIVER_ANY in @chipset, it is treated as compatible with forced
 * name, even when driver was excluded by ANY or is not listed in known drivers.
 *
 * Returns: DRM file descriptor or -1 on error
 */
int __drm_open_device(const char *name, unsigned int chipset)
{
	const char *forced;
	char dev_name[16] = "";
	unsigned int chip = DRIVER_ANY;
	int fd;

	fd = open(name, O_RDWR);
	if (fd == -1)
		return -1;

	if (__get_drm_device_name(fd, dev_name, sizeof(dev_name) - 1) == -1)
		goto err;

	/*
	 * When using forced driver with DRIVER_ANY honor any driver name,
	 * also those excluded from DRIVER_ANY or those that are not listed
	 * in known modules.
	 */
	forced = forced_driver();
	if (forced && chipset == DRIVER_ANY) {
		if (strcmp(forced, dev_name)) {
			igt_debug("Expected driver \"%s\" but got \"%s\"\n",
				  forced, dev_name);
			goto err;
		} else {
			goto opened;
		}
	}

	modulename_to_chipset(dev_name, &chip);

	if ((chipset & chip) != chip)
		goto err;

opened:
	log_opened_device_path(name);
	return fd;

err:
	close(fd);
	return -1;
}

static struct {
	int fd;
	struct stat stat;
}_opened_fds[64];

static int _opened_fds_count;

static void _set_opened_fd(int idx, int fd)
{
	assert(idx < ARRAY_SIZE(_opened_fds));
	assert(idx <= _opened_fds_count);

	_opened_fds[idx].fd = fd;

	assert(fstat(fd, &_opened_fds[idx].stat) == 0);

	_opened_fds_count = idx+1;
}

static bool _is_already_opened(const char *path, int as_idx)
{
	struct stat new;

	assert(as_idx < ARRAY_SIZE(_opened_fds));
	assert(as_idx <= _opened_fds_count);

	/*
	 * we cannot even stat the device, so it's of no use - let's claim it's
	 * already opened
	 */
	if (igt_debug_on(stat(path, &new) != 0))
		return true;

	for (int i = 0; i < as_idx; ++i) {
		/* did we cross filesystem boundary? */
		assert(_opened_fds[i].stat.st_dev == new.st_dev);

		if (_opened_fds[i].stat.st_ino == new.st_ino)
			return true;
	}

	return false;
}

static int __search_and_open(const char *base, int offset, unsigned int chipset, int as_idx)
{
	const char *forced;

	forced = forced_driver();
	if (forced)
		igt_debug("Force option used: Using driver %s\n", forced);

	for (int i = 0; i < 16; i++) {
		char name[80];
		int fd;

		sprintf(name, "%s%u", base, i + offset);

		if (_is_already_opened(name, as_idx))
			continue;

		fd = __drm_open_device(name, chipset);
		if (fd != -1)
			return fd;
	}

	return -1;
}

void drm_load_module(unsigned int chipset)
{
	static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
	const char *forced = forced_driver();
	unsigned int chip = 0;
	bool want_any = chipset == DRIVER_ANY;

	if (forced) {
		if (chipset == DRIVER_VGEM)
			chip = DRIVER_VGEM; /* ignore forced */
		else
			modulename_to_chipset(forced, &chip);

		chipset &= chip; /* forced can be in known modules */
	}

	pthread_mutex_lock(&mutex);
	if (forced && chipset == 0) {
		if (want_any)
			modprobe(forced);
	} else {
		for (const struct module *m = modules; m->module; m++) {
			if (chipset & m->bit) {
				if (m->modprobe)
					m->modprobe(m->module);
				else
					modprobe(m->module);
			}
		}
	}

	pthread_mutex_unlock(&mutex);
	igt_devices_scan();
}

static int __open_driver(const char *base, int offset, unsigned int chipset, int as_idx)
{
	int fd;

	fd = __search_and_open(base, offset, chipset, as_idx);
	if (fd != -1)
		return fd;

	drm_load_module(chipset);

	return __search_and_open(base, offset, chipset, as_idx);
}

static int __open_driver_exact(const char *name, unsigned int chipset)
{
	int fd;

	fd = __drm_open_device(name, chipset);
	if (fd != -1)
		return fd;

	drm_load_module(chipset);

	return __drm_open_device(name, chipset);
}

/*
 * A helper to get the first matching card in case a filter is set.
 * It does all the extra logging around the filters for us.
 *
 * @card: pointer to the igt_device_card structure to be filled
 * when a card is found.
 *
 * Returns:
 * True if card according to the added filter was found,
 * false othwerwise.
 */
static bool __get_card_for_nth_filter(int idx, struct igt_device_card *card)
{
	const char *filter;

	if (igt_device_filter_count() > idx) {
		filter = igt_device_filter_get(idx);
		igt_debug("Looking for devices to open using filter %d: %s\n", idx, filter);

		if (igt_device_card_match(filter, card)) {
			igt_debug("Filter matched %s | %s\n", card->card, card->render);
			return true;
		}
	}

	return false;
}

/**
 * __drm_open_driver_another:
 * @idx: index of the device you are opening
 * @chipset: OR'd flags for each chipset to search, eg. #DRIVER_INTEL
 *
 * This function is intended to be used instead of drm_open_driver() for tests
 * that are opening multiple /dev/dri/card* nodes, usually for the purpose of
 * multi-GPU testing.
 *
 * This function opens device in the following order:
 *
 * 1. when --device arguments are present:
 *   * device scanning is executed,
 *   * idx-th filter (starting with 0, filters are semicolon separated) is used
 *   * if there is no idx-th filter, goto 2
 *   * first device maching the filter is selected
 *   * if it's already opened (for indexes = 0..idx-1) we fail with -1
 *   * otherwise open the device and return the fd
 *
 * 2. compatibility mode - open the first DRM device we can find that is not
 *    already opened for indexes 0..idx-1, searching up to 16 device nodes
 *
 * The test is reponsible to test the interaction between devices in both
 * directions if applicable.
 *
 * Example:
 *
 * |[<!-- language="c" -->
 * igt_subtest_with_dynamic("basic") {
 * 	int first_fd = -1;
 * 	int second_fd = -1;
 *
 * 	first_fd = __drm_open_driver_another(0, DRIVER_ANY);
 * 	igt_require(first_fd >= 0);
 *
 * 	second_fd = __drm_open_driver_another(1, DRIVER_ANY);
 * 	igt_require(second_fd >= 0);
 *
 * 	if (can_do_foo(first_fd, second_fd))
 * 		igt_dynamic("first-to-second")
 * 			test_basic_from_to(first_fd, second_fd);
 *
 * 	if (can_do_foo(second_fd, first_fd))
 * 		igt_dynamic("second-to-first")
 * 			test_basic_from_to(second_fd, first_fd);
 *
 * 	close(first_fd);
 * 	close(second_fd);
 * }
 * ]|
 *
 * Returns:
 * An open DRM fd or -1 on error
 */
int __drm_open_driver_another(int idx, int chipset)
{
	int fd = -1;

	if (chipset != DRIVER_VGEM && igt_device_filter_count() > idx) {
		struct igt_device_card card;
		bool found;

		found = __get_card_for_nth_filter(idx, &card);

		if (!found) {
			drm_load_module(chipset);
			found = __get_card_for_nth_filter(idx, &card);
		}

		if (!found || !strlen(card.card))
			igt_warn("No card matches the filter! [%s]\n",
				 igt_device_filter_get(idx));
		else if (_is_already_opened(card.card, idx))
			igt_warn("card maching filter %d is already opened\n", idx);
		else
			fd = __open_driver_exact(card.card, chipset);

	} else {
		/* no filter for device idx, let's open whatever is available */
		fd = __open_driver("/dev/dri/card", 0, chipset, idx);
	}

	if (fd >= 0) {
		_set_opened_fd(idx, fd);

		/* Cache xe_device struct. */
		if (is_xe_device(fd))
			xe_device_get(fd);
	}

	return fd;
}

/**
 * drm_open_driver_another:
 * @idx: index of the device you are opening
 * @chipset: OR'd flags for each chipset to search, eg. #DRIVER_INTEL
 *
 * A wrapper for __drm_open_driver with skip on fail.
 *
 * Returns:
 * An open DRM fd or skips
 */
int drm_open_driver_another(int idx, int chipset)
{
	int fd = __drm_open_driver_another(idx, chipset);

	igt_skip_on_f(fd < 0, "No known gpu found for chipset flags %d (%s)\n",
		      chipset, chipset_to_str(chipset));

	/* TODO: for i915 and idx > 0 add atomic reset before test */
	return fd;
}

/**
 * __drm_open_driver:
 * @chipset: OR'd flags for each chipset to search, eg. #DRIVER_INTEL
 *
 * Function opens device in the following order:
 * 1. when --device arguments are present device scanning will be executed,
 * then filter argument is used to find matching one.
 * 2. compatibility mode - open the first DRM device we can find,
 * searching up to 16 device nodes.
 *
 * Returns:
 * An open DRM fd or -1 on error
 */
int __drm_open_driver(int chipset)
{
	return __drm_open_driver_another(0, chipset);
}

int __drm_open_driver_render(int chipset)
{
	int fd;

	if (chipset != DRIVER_VGEM && igt_device_filter_count() > 0) {
		struct igt_device_card card;
		bool found;

		found = __get_card_for_nth_filter(0, &card);

		if (!found || !strlen(card.render))
			return -1;

		fd = __open_driver_exact(card.render, chipset);
	} else {
		fd = __open_driver("/dev/dri/renderD", 128, chipset, 0);
	}

	/* Cache xe_device struct. */
	if (fd >= 0 && is_xe_device(fd))
		xe_device_get(fd);

	return fd;
}

static int at_exit_drm_fd = -1;
static int at_exit_drm_render_fd = -1;

static void __cancel_work_at_exit(int fd)
{
	igt_terminate_spins(); /* for older kernels */

	igt_params_set(fd, "reset", "%u", -1u /* any method */);
	igt_drop_caches_set(fd,
			    /* cancel everything */
			    DROP_RESET_ACTIVE | DROP_RESET_SEQNO |
			    /* cleanup */
			    DROP_ACTIVE | DROP_RETIRE | DROP_IDLE | DROP_FREED);
}

static void cancel_work_at_exit(int sig)
{
	if (at_exit_drm_fd < 0)
		return;

	__cancel_work_at_exit(at_exit_drm_fd);

	close(at_exit_drm_fd);
	at_exit_drm_fd = -1;
}

static void cancel_work_at_exit_render(int sig)
{
	if (at_exit_drm_render_fd < 0)
		return;

	__cancel_work_at_exit(at_exit_drm_render_fd);

	close(at_exit_drm_render_fd);
	at_exit_drm_render_fd = -1;
}

static const char *chipset_to_vendor_str(int chipset)
{
	return chipset == DRIVER_XE ? chipset_to_str(DRIVER_INTEL) : chipset_to_str(chipset);
}

/**
 * drm_open_driver:
 * @chipset: OR'd flags for each chipset to search, eg. #DRIVER_INTEL
 *
 * Open a drm legacy device node. This function always returns a valid
 * file descriptor.
 *
 * Returns: a drm file descriptor
 */
int drm_open_driver(int chipset)
{
	static int open_count;
	int fd;

	fd = __drm_open_driver(chipset);
	igt_skip_on_f(fd<0, "No known gpu found for chipset flags 0x%u (%s)\n",
		      chipset, chipset_to_str(chipset));

	/* For i915, at least, we ensure that the driver is idle before
	 * starting a test and we install an exit handler to wait until
	 * idle before quitting.
	 */
	if (is_i915_device(fd)) {
		if (__sync_fetch_and_add(&open_count, 1) == 0) {
			__cancel_work_at_exit(fd);
			at_exit_drm_fd = drm_reopen_driver(fd);
			igt_install_exit_handler(cancel_work_at_exit);
		}
	}

	return fd;
}

static bool is_valid_fd(int fd)
{
	char path[32];
	char buf[PATH_MAX];
	int len;

	if (fd < 0)
		return false;

	snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);

	memset(buf, 0, sizeof(buf));
	len = readlink(path, buf, sizeof(buf) - 1);
	if (len <= 0)
		return false;

	buf[len] = '\0';
	if (strstr(buf, "/dev/dri/card") == buf ||
	    strstr(buf, "/dev/dri/renderD") == buf)
		return true;

	return false;
}

/**
 * __drm_close_driver:
 * @fd: a drm file descriptor
 *
 * Check the given drm file descriptor @fd is valid and if not,
 * return -1. For valid fd close it and make cleanups.
 *
 * Returns: 0 on success or -1 on error.
 */
int __drm_close_driver(int fd)
{
	if (!is_valid_fd(fd))
		return -1;

	/* Remove xe_device from cache. */
	if (is_xe_device(fd))
		xe_device_put(fd);

	return close(fd);
}

/**
 * drm_close_driver:
 * @fd: a drm file descriptor
 *
 * Check the given drm file descriptor @fd is valid and if not issue warning.
 * For valid fd close it and make cleanups.
 *
 * Returns: 0 on success or -1 on error.
 */
int drm_close_driver(int fd)
{
	if (!is_valid_fd(fd)) {
		igt_warn("Don't attempt to close standard/invalid file "
			 "descriptor: %d\n", fd);
		return -1;
	}

	return __drm_close_driver(fd);
}

/**
 * drm_open_driver_master:
 * @chipset: OR'd flags for each chipset to search, eg. #DRIVER_INTEL
 *
 * Open a drm legacy device node and ensure that it is drm master.
 *
 * Returns:
 * The drm file descriptor or -1 on error
 */
int drm_open_driver_master(int chipset)
{
	int fd = drm_open_driver(chipset);

	igt_device_set_master(fd);

	return fd;
}

/**
 * drm_open_driver_render:
 * @chipset: OR'd flags for each chipset to search, eg. #DRIVER_INTEL
 *
 * Open a drm render device node.
 *
 * Returns:
 * The drm file descriptor or -1 on error
 */
int drm_open_driver_render(int chipset)
{
	static int open_count;
	int fd = __drm_open_driver_render(chipset);

	/* no render nodes, fallback to drm_open_driver() */
	if (fd == -1)
		return drm_open_driver(chipset);

	if (__sync_fetch_and_add(&open_count, 1))
		return fd;

	at_exit_drm_render_fd = drm_reopen_driver(fd);
	if (chipset & DRIVER_INTEL) {
		__cancel_work_at_exit(fd);
		igt_install_exit_handler(cancel_work_at_exit_render);
	}

	return fd;
}

/**
 * drm_reopen_driver:
 * @fd: re-open the drm file descriptor
 *
 * Re-opens the drm fd which is useful in instances where a clean default
 * context is needed.
 */
int drm_reopen_driver(int fd)
{
	char path[256];

	snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
	fd = open(path, O_RDWR);
	igt_assert_fd(fd);

	if (is_xe_device(fd))
		xe_device_get(fd);

	return fd;
}

int drm_prepare_filtered_multigpu(int chipset)
{
	const char *vendor = chipset_to_vendor_str(chipset);

	return igt_device_prepare_filtered_view(vendor);
}

/**
 * drm_open_filtered_card:
 * @idx: index for GPU to open
 *
 * Open N-th GPU from filtered list
 *
 * Returns:
 * Opened device or -1 if error.
 */
int drm_open_filtered_card(int idx)
{
	struct igt_device_card card;
	const char *filter;
	int fd = -1;

	if (idx < 0 || idx >= igt_device_filter_count()) {
		igt_debug("Invalid filter index %d\n", idx);
		return -1;
	}

	filter = igt_device_filter_get(idx);
	if (igt_device_card_match(filter, &card))
		fd = igt_open_card(&card);

	if (fd >= 0) {
		igt_debug("Opened GPU%d card: %s\n", idx, card.card);
		log_opened_device_path(card.card);
		/* Cache xe_device struct. */
		if (is_xe_device(fd))
			xe_device_get(fd);
	} else {
		igt_debug("Opening GPU%d failed, card: %s\n", idx, card.card);
	}

	return fd;
}

void igt_require_amdgpu(int fd)
{
	igt_require(is_amdgpu_device(fd));
}

void igt_require_intel(int fd)
{
	igt_require(is_intel_device(fd));
}

void igt_require_i915(int fd)
{
	igt_require(is_i915_device(fd));
}

void igt_require_nouveau(int fd)
{
	igt_require(is_nouveau_device(fd));
}

void igt_require_vc4(int fd)
{
	igt_require(is_vc4_device(fd));
}

void igt_require_xe(int fd)
{
	igt_require(is_xe_device(fd));
}

void igt_require_vkms(void)
{
	/*
	 * Since VKMS can create and destroy virtual drivers at will, instead
	 * look to make sure the driver is installed.
	 */
	struct stat s = {};
	int ret;
	const char *vkms_module_dir = "/sys/module/vkms";

	ret = stat(vkms_module_dir, &s);

	igt_require_f(ret == 0, "VKMS stat of %s returned %d (%s)\n",
		      vkms_module_dir, ret, strerror(ret));
	igt_require_f(S_ISDIR(s.st_mode),
		      "VKMS stat of %s was not a directory\n", vkms_module_dir);
}
