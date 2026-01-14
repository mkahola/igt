// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <pciaccess.h>
#include "igt_aux.h"
#include "igt_core.h"
#include "igt_pci.h"
#include "igt_sysfs.h"

static int find_pci_cap_offset_at(struct pci_device *dev, enum pci_cap_id cap_id,
				  int start_offset)
{
	uint8_t offset = 0xff;
	uint16_t cap_header = 0xffff;
	int loop = (PCI_CFG_SPACE_SIZE - PCI_TYPE0_1_HEADER_SIZE)
			/ sizeof(cap_header);

	if (pci_device_cfg_read_u8(dev, &offset, start_offset))
		return -1;

	while (loop--) {
		igt_assert_f(offset != 0xff, "pci config space inaccessible\n");

		if (offset < PCI_TYPE0_1_HEADER_SIZE)
			break;

		if (pci_device_cfg_read_u16(dev, &cap_header, (offset & 0xFC)))
			return -1;

		if (!cap_id || cap_id == (cap_header & 0xFF))
			return offset;

		offset = cap_header >> 8;
	}

	igt_fail_on_f(loop <= 0 && offset, "pci capability offset doesn't terminate\n");

	return 0;
}

/**
 * find_pci_cap_offset:
 * @dev: pci device
 * @cap_id: searched capability id, 0 means any capability
 *
 * return:
 * -1 on config read error, 0 if capability is not found,
 * otherwise offset at which capability with cap_id is found
 */
int find_pci_cap_offset(struct pci_device *dev, enum pci_cap_id cap_id)
{
	return find_pci_cap_offset_at(dev, cap_id, PCI_CAPS_START);
}

static int open_pci_driver_dir(const char *driver)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "/sys/bus/pci/drivers/%s", driver);
	return open(path, O_RDONLY | O_CLOEXEC);
}

/**
 * igt_pci_device_unbind:
 * @pci_slot: BDF like "0000:01:00.0"
 *
 * Unbind @pci_slot from its currently bound driver, if any.
 * Returns 0 on success, or a negative errno-like value.
 */
int igt_pci_device_unbind(const char *pci_slot)
{
	char path[PATH_MAX];
	int dirfd;
	int ret;

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/driver", pci_slot);
	dirfd = open(path, O_RDONLY | O_CLOEXEC);
	if (dirfd < 0)
		return 0; /* already unbound */

	ret = igt_sysfs_set(dirfd, "unbind", pci_slot) ? 0 : -errno;
	close(dirfd);

	return ret;
}

/**
 * igt_pci_driver_bind:
 * @driver: PCI driver name under /sys/bus/pci/drivers/<driver>
 * @pci_slot: device to bind
 *
 * Bind @pci_slot to @driver. Driver must be present/loaded.
 * Returns 0 on success, or a negative errno-like value.
 */
int igt_pci_driver_bind(const char *driver, const char *pci_slot)
{
	int dirfd, ret;

	dirfd = open_pci_driver_dir(driver);
	if (dirfd < 0)
		return -errno;

	ret = igt_sysfs_set(dirfd, "bind", pci_slot) ? 0 : -errno;
	close(dirfd);

	return ret;
}

/**
 * igt_pci_driver_unbind:
 * @driver: PCI driver name
 * @pci_slot: device to unbind
 *
 * Unbind @pci_slot from @driver.
 * Returns 0 on success, or a negative errno-like value.
 */
int igt_pci_driver_unbind(const char *driver, const char *pci_slot)
{
	int dirfd, ret;

	dirfd = open_pci_driver_dir(driver);
	if (dirfd < 0)
		return -errno;

	ret = igt_sysfs_set(dirfd, "unbind", pci_slot) ? 0 : -errno;
	close(dirfd);

	return ret;
}

/**
 * igt_pci_driver_unbind_all:
 * @driver: PCI driver name
 *
 * Unbind all devices currently bound to @driver.
 * Returns 0 on success, or a negative errno-like value.
 */
int igt_pci_driver_unbind_all(const char *driver)
{
	char path[PATH_MAX];
	DIR *dir;
	struct dirent *de;
	int driver_fd;

	snprintf(path, sizeof(path), "/sys/bus/pci/drivers/%s", driver);
	dir = opendir(path);
	if (!dir)
		return -errno;

	driver_fd = dirfd(dir);

	while ((de = readdir(dir))) {
		bool ok;

		/* BDF symlinks are like "0000:01:00.0" and start with digit */
		if (de->d_type != DT_LNK || !isdigit(de->d_name[0]))
			continue;

		ok = igt_sysfs_set(driver_fd, "unbind", de->d_name);
		if (!ok) {
			int err = -errno;

			closedir(dir);
			return err;
		}
	}

	closedir(dir);
	return 0;
}

/**
 * igt_pci_set_driver_override:
 * @pci_slot: PCI device BDF (e.g. "0000:01:00.0")
 * @driver: PCI driver name to force-bind (e.g. "xe-vfio-pci"), or
 *          NULL / empty string to clear an existing override
 *
 * Set or clear the PCI driver_override for @pci_slot via sysfs.
 *
 * This does not trigger driver reprobe by itself. Call
 * igt_pci_probe_drivers() afterwards to apply the override.
 *
 * Returns: 0 on success, negative errno on failure.
 */
int igt_pci_set_driver_override(const char *pci_slot, const char *driver)
{
	char devpath[PATH_MAX];
	int dev;
	bool ok;

	snprintf(devpath, sizeof(devpath), "/sys/bus/pci/devices/%s", pci_slot);
	dev = open(devpath, O_DIRECTORY | O_RDONLY);
	if (dev < 0)
		return -errno;

	ok = igt_sysfs_set(dev, "driver_override", driver ? driver : "");
	close(dev);

	return ok ? 0 : -errno;
}

/**
 * igt_pci_probe_drivers:
 * @pci_slot: PCI device BDF (e.g. "0000:01:00.0")
 *
 * Trigger PCI driver reprobe for @pci_slot by writing to
 * /sys/bus/pci/drivers_probe.
 *
 * This causes the kernel to attempt binding the device, honoring any
 * driver_override previously set.
 *
 * Note: a successful write only means the reprobe request was accepted.
 * It does not guarantee that a driver actually bound to the device.
 *
 * Returns: 0 on success, negative errno on failure.
 */
int igt_pci_probe_drivers(const char *pci_slot)
{
	int pci;
	bool ok;

	pci = open("/sys/bus/pci", O_DIRECTORY | O_RDONLY);
	if (pci < 0)
		return -errno;

	ok = igt_sysfs_set(pci, "drivers_probe", pci_slot);
	close(pci);

	return ok ? 0 : -errno;
}

/**
 * igt_pci_get_bound_driver_name:
 * @pci_slot: PCI device BDF (e.g. "0000:01:00.0")
 * @driver: destination buffer for the bound driver name
 * @driver_len: size of @driver in bytes (may be 0 if @driver is NULL)
 *
 * Read the currently bound PCI driver name for @pci_slot by inspecting the
 * /sys/bus/pci/devices/<BDF>/driver symlink.
 *
 * @driver/@driver_len are optional. Callers may pass NULL and/or 0 when they
 * only need the bound/unbound status and do not care about the driver name.
 *
 * Return values:
 *  1: device is bound and @driver contains the driver name
 *  0: device is unbound (no driver symlink)
 * <0: negative errno-like value on error
 */
int igt_pci_get_bound_driver_name(const char *pci_slot, char *driver, size_t driver_len)
{
	char path[PATH_MAX];
	char link[PATH_MAX];
	const char *base;
	ssize_t len;

	if (driver && driver_len)
		driver[0] = '\0';

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/driver", pci_slot);
	len = readlink(path, link, sizeof(link) - 1);
	if (len < 0) {
		if (errno == ENOENT)
			return 0; /* unbound */

		return -errno;
	}

	link[len] = '\0';
	base = strrchr(link, '/');
	base = base ? base + 1 : link;

	if (driver && driver_len)
		snprintf(driver, driver_len, "%s", base);

	return 1;
}

/**
 * igt_pci_bind_driver_override:
 * @pci_slot: PCI device BDF (e.g. "0000:01:00.0")
 * @driver: PCI driver name to bind (must not be NULL or empty)
 * @timeout_ms: how long to wait for the device to become bound.
 *              If 0, don't wait (best-effort immediate check only).
 *
 * Bind @pci_slot to @driver using the driver_override mechanism.
 *
 * This helper sets driver_override and immediately triggers driver
 * reprobe so that the device is bound to the requested driver.
 *
 * Returns: 0 on success, negative errno-like value on failure.
 * A reprobe request can be accepted by sysfs while the driver probe
 * fails later; this helper verifies the device ended up bound.
 *
 * On bind failure, returns a negative error and the failure reason may
 * also be logged to dmesg by the kernel driver.
 */
int igt_pci_bind_driver_override(const char *pci_slot, const char *driver,
				 unsigned int timeout_ms)
{
	int ret;
	char bound[64];
	int bound_ret;
	bool bound_ok;

	if (!driver || !driver[0])
		return -EINVAL;

	ret = igt_pci_set_driver_override(pci_slot, driver);
	if (ret)
		return ret;

	ret = igt_pci_probe_drivers(pci_slot);
	if (ret)
		return ret;

	/*
	 * Writing to drivers_probe only tells us the kernel accepted the request.
	 * The actual driver probe may still fail (and only be reported via dmesg).
	 * Verify that the device ended up bound to the requested driver.
	 */
	bound_ret = igt_pci_get_bound_driver_name(pci_slot, bound, sizeof(bound));
	if (bound_ret < 0)
		return bound_ret;

	if (timeout_ms == 0) {
		/*
		 * No waiting requested. If the device is already bound, validate
		 * it is bound to the expected driver; otherwise treat as
		 * best-effort request-only success.
		 */
		if (bound_ret > 0 && strcmp(bound, driver))
			return -EBUSY;

		return 0;
	}

	bound_ok = igt_wait((bound_ret =
			     igt_pci_get_bound_driver_name(pci_slot, bound, sizeof(bound))) != 0,
			    timeout_ms, 1);
	if (!bound_ok)
		return -EIO;

	if (bound_ret < 0)
		return bound_ret;

	if (strcmp(bound, driver))
		return -EBUSY;

	return 0;
}

/**
 * igt_pci_unbind_driver_override:
 * @pci_slot: PCI device BDF (e.g. "0000:01:00.0")
 * @timeout_ms: how long to wait for the device to become unbound.
 *              If 0, don't wait (best-effort immediate check only).
 *
 * Unbind @pci_slot from its currently bound driver (if any) and clear
 * any driver_override setting.
 *
 * This is the inverse operation of igt_pci_bind_driver_override().
 *
 * Returns: 0 on success, negative errno on failure.
 */
int igt_pci_unbind_driver_override(const char *pci_slot, unsigned int timeout_ms)
{
	int ret;
	int bound_ret;
	char bound[64];
	bool unbound_ok;

	ret = igt_pci_device_unbind(pci_slot);
	if (ret)
		return ret;

	ret = igt_pci_set_driver_override(pci_slot, "");
	if (ret)
		return ret;

	bound_ret = igt_pci_get_bound_driver_name(pci_slot, bound, sizeof(bound));
	if (bound_ret < 0)
		return bound_ret;

	if (timeout_ms == 0)
		return 0;

	/* Verify the device actually ends up unbound (driver symlink removed). */
	unbound_ok = igt_wait((bound_ret =
			       igt_pci_get_bound_driver_name(pci_slot, bound, sizeof(bound))) == 0,
			      timeout_ms, 1);
	if (!unbound_ok)
		return -EBUSY;

	if (bound_ret < 0)
		return bound_ret;

	return 0;
}
