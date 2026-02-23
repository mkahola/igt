/*
 * Copyright © 2019 Intel Corporation
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

#include "drmtest.h"
#include "igt_aux.h"
#include "igt_core.h"
#include "igt_device_scan.h"
#include "igt_list.h"
#include "igt_map.h"
#include "intel_chipset.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <libudev.h>
#ifdef __linux__
#include <linux/limits.h>
#include <linux/pci_regs.h>
#endif
#ifndef PCI_HEADER_TYPE_MASK
/* Either not linux, or <linux/pci_regs.h> too old */
#define PCI_HEADER_TYPE_MASK 0x7f
#endif
#include <pci/pci.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

/**
 * SECTION:igt_device_scan
 * @short_description: Device scanning and selection
 * @title: Device selection
 * @include: igt.h
 *
 * # Device scanning
 *
 * Device scanning iterates over DRM subsystem using udev library
 * to acquire DRM devices.
 * For each DRM device we also get and store its parent to allow device
 * selection happen in a more contextual way.
 *
 * Parent devices are bus devices (like PCI, platform, etc.) and contain a lot
 * of extra data on top of the DRM device itself.
 *
 * # Filters
 *
 * Device selection can be done using filters that are using the data collected
 * udev + some syntactic sugar.
 *
 * Direct device selection filter uses sysfs path to find the device:
 *
 * |[<!-- language="plain" -->
 * sys:/sys/path/to/device/or/parent
 * ]|
 *
 * Examples:
 * |[<!-- language="plain" -->
 * - sys:/sys/devices/pci0000:00/0000:00:02.0/drm/card0
 * - sys:/sys/devices/pci0000:00/0000:00:02.0
 * - sys:/sys/devices/platform/vgem
 * ]|
 *
 * The alternative is to use other implemented filters:
 *
 * - drm: get drm /dev/dri/... device directly
 *
 *   |[<!-- language="plain" -->
 *   drm:/dev/dri/...
 *   ]|
 *
 *   Loading drivers in different order can cause different ordering of
 *   /dev/dri/card nodes which be problematic for reliable and reproducible
 *   device selection, e.g. in automated execution setting. In such scenarios
 *   please consider using sys, pci or platform filters instead.
 *
 * - pci: select device using PCI slot or vendor and device properties
 *   |[<!-- language="plain" -->
 *   pci:[vendor=%04x/name][,device=%04x/codename][,card=%d] | [slot=%04x:%02x:%02x.%x]
 *   ]|
 *
 *   Filter allows device selection using vendor (hex or name), device id
 *   (hex or codename) and nth-card from all matches. For example if there
 *   are 4 PCI cards installed (let two cards have 1234 and other two 1235
 *   device id, all of them of vendor Intel) you can select one using:
 *
 *   |[<!-- language="plain" -->
 *   pci:vendor=Intel,device=1234,card=0
 *   ]|
 *
 *   or
 *
 *   |[<!-- language="plain" -->
 *   pci:vendor=8086,device=1234,card=0
 *   ]|
 *
 *   This takes first device with 1234 id for Intel vendor (8086).
 *
 *   |[<!-- language="plain" -->
 *   pci:vendor=Intel,device=1234,card=1
 *   ]|
 *
 *   or
 *
 *   |[<!-- language="plain" -->
 *   pci:vendor=8086,device=1234,card=1
 *   ]|
 *
 *   It selects the second one.
 *
 *   |[<!-- language="plain" -->
 *   pci:vendor=8086,device=1234,card=all
 *   pci:vendor=8086,device=1234,card=*
 *   ]|
 *
 *   This will add 0..N card selectors, where 0 <= N <= 63. At least one
 *   filter will be added with card=0 and all incrementally matched ones
 *   up to max numbered 63 (max total 64).
 *
 *   We may use device codename or pseudo-codename (integrated/discrete)
 *   instead of pci device id:
 *
 *   |[<!-- language="plain" -->
 *   pci:vendor=8086,device=skylake
 *   ]|
 *
 *   or
 *
 *   |[<!-- language="plain" -->
 *   pci:vendor=8086,device=integrated
 *   ]|
 *
 *   Another possibility is to select device using a PCI slot:
 *
 *   |[<!-- language="plain" -->
 *   pci:slot=0000:01:00.0
 *   ]|
 *
 *   As order the on PCI bus doesn't change (unless you'll add new device or
 *   reorder existing one) device selection using this filter will always
 *   return you same device regardless the order of enumeration.
 *
 *   Simple syntactic sugar over using the sysfs paths.
 *
 * - sriov: select pf or vf
 *   |[<!-- language="plain" -->
 *   sriov:[vendor=%04x/name][,device=%04x/codename][,card=%d][,pf=%d][,vf=%d]
 *   ]|
 *
 *   Filter extends pci selector to allow pf/vf selection:
 *
 *   |[<!-- language="plain" -->
 *   sriov:vendor=Intel,device=1234,card=0,vf=2
 *   ]|
 *
 *   When vf is not defined, pf will be selected:
 *
 *   |[<!-- language="plain" -->
 *   sriov:vendor=Intel,device=1234,card=0
 *   ]|
 *
 *   In case a device has more than one pf, you can also select a specific pf
 *   or a vf associated with a specific pf:
 *
 *   |[<!-- language="plain" -->
 *   sriov:vendor=Intel,device=1234,card=0,pf=1
 *   ]|
 *
 *   |[<!-- language="plain" -->
 *   sriov:vendor=Intel,device=1234,card=0,pf=1,vf=0
 *   ]|
 *
 */

#ifdef DEBUG_DEVICE_SCAN
#define DBG(...) \
{ \
	struct timeval tm; \
	gettimeofday(&tm, NULL); \
	printf("%10ld.%06ld: ", tm.tv_sec, tm.tv_usec); \
	printf(__VA_ARGS__); \
}

#else
#define DBG(...) {}
#endif

enum dev_type {
	DEVTYPE_ALL,
	DEVTYPE_INTEGRATED,
	DEVTYPE_DISCRETE,
};

#define STR_INTEGRATED "integrated"
#define STR_DISCRETE "discrete"

static const char * const attrs[] = { "driver", "sriov_numvfs", "physfn" };

static inline bool strequal(const char *a, const char *b)
{
	if (a == NULL || b == NULL)
		return false;

	return strcmp(a, b) == 0;
}

struct igt_device {
	/* Filled for drm devices */
	struct igt_device *parent;

	/* Point to vendor spec if can be found */

	/* Properties / sysattrs rewriten from udev lists */
	struct igt_map *props_map;
	struct igt_map *attrs_map;

	/* Most usable variables from udev device */
	char *subsystem;
	char *syspath;
	char *devnode;
	char *sysname;

	/* /dev/dri/... paths */
	char *drm_card;
	char *drm_render;

	/* For pci subsystem */
	char *vendor;
	char *device;
	char *pci_slot_name;
	char *driver;
	int gpu_index; /* For more than one GPU with same vendor and device. */

	char *codename; /* For grouping by codename */
	enum dev_type dev_type; /* For grouping by integrated/discrete */

	char *pci_gpu; /* Filled for upstream bridge ports */

	struct igt_list_head link;
};

/* Scanned devices */
static __thread struct {
	struct igt_list_head all;
	struct igt_list_head filtered;
	bool devs_scanned;
} igt_devs;

static void igt_device_free(struct igt_device *dev);

typedef char *(*devname_fn)(uint16_t, uint16_t);
typedef enum dev_type (*devtype_fn)(uint16_t, uint16_t, const char *);

static char *devname_hex(uint16_t vendor, uint16_t device)
{
	char *s;

	igt_assert(asprintf(&s, "%04x:%04x", vendor, device) == 9);

	return s;
}

static char *devname_intel(uint16_t vendor, uint16_t device)
{
	const struct intel_device_info *info = intel_get_device_info(device);
	char *devname = NULL, *s;

	if (info->codename) {
		devname = strdup(info->codename);

		if (devname) {
			devname[0] = toupper(devname[0]);
			igt_assert(asprintf(&s, "Intel %s (Gen%u)", devname,
					    info->graphics_ver) != -1);
			free(devname);
		}
	}

	if (!devname)
		s = devname_hex(vendor, device);

	return s;
}

static char *codename_intel(uint16_t vendor, uint16_t device)
{
	const struct intel_device_info *info = intel_get_device_info(device);
	char *codename = NULL;

	if (info->codename) {
		codename = strdup(info->codename);
		igt_assert(codename);
	}

	if (!codename)
		codename = devname_hex(vendor, device);

	return codename;
}

static enum dev_type devtype_intel(uint16_t vendor, uint16_t device, const char *pci_slot)
{
	(void) vendor;
	(void) device;

	if (!strncmp(pci_slot, INTEGRATED_I915_GPU_PCI_ID, PCI_SLOT_NAME_SIZE))
		return DEVTYPE_INTEGRATED;

	return DEVTYPE_DISCRETE;
}

static enum dev_type devtype_all(uint16_t vendor, uint16_t device, const char *pci_slot)
{
	(void) vendor;
	(void) device;
	(void) pci_slot;

	return DEVTYPE_ALL;
}

static struct {
	const char *name;
	const char *vendor_id;
	devname_fn devname;
	devname_fn codename;
	devtype_fn devtype;
} pci_vendor_mapping[] = {
	{ "intel", "8086", devname_intel, codename_intel, devtype_intel },
	{ "amd", "1002", devname_hex, devname_hex, devtype_all },
	{ NULL, },
};

static const char *get_pci_vendor_id_by_name(const char *name)
{
	for (typeof(*pci_vendor_mapping) *vm = pci_vendor_mapping; vm->name; vm++)
	{
		if (!strcasecmp(name, vm->name))
			return vm->vendor_id;
	}

	return NULL;
}

static devname_fn get_pci_vendor_device_fn(uint16_t vendor)
{
	char vendorstr[5];

	snprintf(vendorstr, sizeof(vendorstr), "%04x", vendor);

	for (typeof(*pci_vendor_mapping) *vm = pci_vendor_mapping; vm->name; vm++) {
		if (!strcasecmp(vendorstr, vm->vendor_id))
			return vm->devname;
	}

	return devname_hex;
}

static devname_fn get_pci_vendor_device_codename_fn(uint16_t vendor)
{
	char vendorstr[5];

	snprintf(vendorstr, sizeof(vendorstr), "%04x", vendor);

	for (typeof(*pci_vendor_mapping) *vm = pci_vendor_mapping; vm->name; vm++) {
		if (!strcasecmp(vendorstr, vm->vendor_id))
			return vm->codename;
	}

	return devname_hex;
}

static devtype_fn get_pci_vendor_device_devtype_fn(uint16_t vendor)
{
	char vendorstr[5];

	snprintf(vendorstr, sizeof(vendorstr), "%04x", vendor);

	for (typeof(*pci_vendor_mapping) *vm = pci_vendor_mapping; vm->name; vm++) {
		if (!strcasecmp(vendorstr, vm->vendor_id))
			return vm->devtype;
	}

	return devtype_all;
}

static void get_pci_vendor_device(const struct igt_device *dev,
				  uint16_t *vendorp, uint16_t *devicep)
{
	igt_assert(dev && dev->vendor && dev->device);
	igt_assert(vendorp && devicep);

	igt_assert(sscanf(dev->vendor, "%hx", vendorp) == 1);
	igt_assert(sscanf(dev->device, "%hx", devicep) == 1);
}

static char *__pci_pretty_name(uint16_t vendor, uint16_t device, bool numeric)
{
	devname_fn fn;

	if (!numeric)
		fn = get_pci_vendor_device_fn(vendor);
	else
		fn = devname_hex;

	return fn(vendor, device);
}

static char *__pci_codename(uint16_t vendor, uint16_t device)
{
	devname_fn fn;

	fn = get_pci_vendor_device_codename_fn(vendor);

	return fn(vendor, device);
}

static enum dev_type __pci_devtype(uint16_t vendor, uint16_t device, const char *pci_slot)
{
	devtype_fn fn;

	fn = get_pci_vendor_device_devtype_fn(vendor);

	return fn(vendor, device, pci_slot);
}

/* Reading sysattr values can take time (even seconds),
 * we want to avoid reading such keys.
 */
static bool is_on_blacklist(const char *what)
{
	static const char *keys[] = { "config", "modalias", "modes",
				      "resource",
				      "resource0", "resource1", "resource2",
				      "resource3", "resource4", "resource5",
				      "resource0_wc", "resource1_wc", "resource2_wc",
				      "resource3_wc", "resource4_wc", "resource5_wc",
				      "uevent", NULL};
	const char *key;
	int i = 0;

	if (what == NULL)
		return false;

	/* Skip attributes in subdirectories */
	if (strchr(what, '/') != NULL)
		return true;

	while ((key = keys[i++])) {
		if (strcmp(key, what) == 0)
			return true;
	}

	return false;

}

static int key_equals(const void *key1, const void *key2)
{
	return strcmp((char *)key1, (char *)key2) == 0;
}

static struct igt_device *igt_device_new(void)
{
	struct igt_device *dev;

	dev = calloc(1, sizeof(struct igt_device));
	if (!dev)
		return NULL;

	dev->attrs_map = igt_map_create(igt_map_hash_32, key_equals);
	dev->props_map = igt_map_create(igt_map_hash_32, key_equals);

	if (dev->attrs_map && dev->props_map)
		return dev;

	igt_map_destroy(dev->attrs_map, NULL);
	igt_map_destroy(dev->props_map, NULL);
	free(dev);

	return NULL;
}

static void igt_device_add_prop(struct igt_device *dev,
				const char *key, const char *value)
{
	if (!key || !value)
		return;

	igt_map_insert(dev->props_map, strdup(key), strdup(value));
}

static void igt_device_add_attr(struct igt_device *dev,
				const char *key, const char *value)
{
	char linkto[PATH_MAX];
	const char *v = value;

	if (!key)
		return;

	/* It's possible we have symlink at key filename, but udev
	 * library resolves only few of them
	 */
	if (!v) {
		struct stat st;
		char path[PATH_MAX];
		int len;

		snprintf(path, sizeof(path), "%s/%s", dev->syspath, key);
		if (lstat(path, &st) != 0)
			return;

		len = readlink(path, linkto, sizeof(linkto));
		if (len <= 0 || len == (ssize_t) sizeof(linkto))
			return;
		linkto[len] = '\0';
		v = strrchr(linkto, '/');
		if (v == NULL)
			return;
		v++;
	}

	igt_map_insert(dev->attrs_map, strdup(key), strdup(v));
}

/* Iterate over udev properties list and rewrite it to igt_device properties
 * hash table for instant access.
 */
static void get_props(struct udev_device *dev, struct igt_device *idev)
{
	struct udev_list_entry *entry;

	entry = udev_device_get_properties_list_entry(dev);
	while (entry) {
		const char *name = udev_list_entry_get_name(entry);
		const char *value = udev_list_entry_get_value(entry);

		igt_device_add_prop(idev, name, value);
		entry = udev_list_entry_get_next(entry);
		DBG("prop: %s, val: %s\n", name, value);
	}
}

/* Same as get_props(), but rewrites sysattrs. Resolves symbolic links
 * not handled by udev get_sysattr_value().
 * Function skips sysattrs from blacklist ht (acquiring some values can take
 * seconds).
 */
static void get_attrs_all(struct udev_device *dev, struct igt_device *idev)
{
	struct udev_list_entry *entry;

	entry = udev_device_get_sysattr_list_entry(dev);
	while (entry) {
		const char *key = udev_list_entry_get_name(entry);
		const char *value;

		if (is_on_blacklist(key)) {
			entry = udev_list_entry_get_next(entry);
			continue;
		}

		value = udev_device_get_sysattr_value(dev, key);
		igt_device_add_attr(idev, key, value);
		entry = udev_list_entry_get_next(entry);
		DBG("attr: %s, val: %s\n", key, value);
	}
}

static void get_attrs_limited(struct udev_device *dev, struct igt_device *idev)
{
	const char *value;

	for (int i = 0; i < ARRAY_SIZE(attrs); i++) {
		value = udev_device_get_sysattr_value(dev, attrs[i]);
		igt_device_add_attr(idev, attrs[i], value);
		DBG("attr: %s, val: %s\n", attrs[i], value);
	}
}

#define get_prop(dev, prop) ((char *) igt_map_search((dev)->props_map, prop))
#define get_attr(dev, attr) ((char *) igt_map_search((dev)->attrs_map, attr))
#define get_prop_subsystem(dev) get_prop(dev, "SUBSYSTEM")
#define is_drm_subsystem(dev)  (strequal(get_prop_subsystem(dev), "drm"))
#define is_pci_subsystem(dev)  (strequal(get_prop_subsystem(dev), "pci"))

static inline void _print_key_value(const char *k, const char *v)
{
	printf("%-32s: %s\n", k, v);
}

static bool is_link_attr(const char *name)
{
	return !strcmp(name, "max_link_speed") ||
	       !strcmp(name, "max_link_width") ||
	       !strcmp(name, "current_link_speed") ||
	       !strcmp(name, "current_link_width");
}

static bool is_aer_attr(const char *name)
{
	return !strcmp(name, "aer_dev_correctable") ||
	       !strcmp(name, "aer_dev_nonfatal") ||
	       !strcmp(name, "aer_dev_fatal");
}

static void dump_props_and_attrs(const struct igt_device *dev, bool omit_link)
{
	struct igt_map_entry *entry;

	printf("\n[properties]\n");
	igt_map_foreach(dev->props_map, entry) {
		_print_key_value((char *)entry->key, (char *)entry->data);
	}

	printf("\n[attributes]\n");
	igt_map_foreach(dev->attrs_map, entry) {
		/* omit link bandwidth attributes if requested */
		if (omit_link && is_link_attr(entry->key))
			continue;

		/* omit multi-line AER statistics data */
		if (is_aer_attr(entry->key))
			continue;

		_print_key_value((char *)entry->key, (char *)entry->data);
	}
	printf("\n");
}

/*
 * Get PCI_SLOT_NAME property, it should be in format of
 * xxxx:yy:zz.z
 */
static bool set_pci_slot_name(struct igt_device *dev)
{
	const char *pci_slot_name = get_prop(dev, "PCI_SLOT_NAME");

	if (!pci_slot_name || strlen(pci_slot_name) != PCI_SLOT_NAME_SIZE)
		return false;

	dev->pci_slot_name = strdup(pci_slot_name);
	return true;
}

/*
 * Gets PCI_ID property, splits to xxxx:yyyy and stores
 * xxxx to dev->vendor and yyyy to dev->device for
 * faster access.
 */
static bool set_vendor_device(struct igt_device *dev)
{
	const char *pci_id = get_prop(dev, "PCI_ID");

	if (!pci_id || strlen(pci_id) != 9)
		return false;

	dev->vendor = strndup(pci_id, 4);
	dev->device = strndup(pci_id + 5, 4);

	return true;
}

/* Initialize lists for keeping scanned devices */
static bool prepare_scan(void)
{
	IGT_INIT_LIST_HEAD(&igt_devs.all);
	IGT_INIT_LIST_HEAD(&igt_devs.filtered);

	return true;
}

static char* strdup_nullsafe(const char* str)
{
	if (str == NULL)
		return NULL;

	return strdup(str);
}

/* Create new igt_device from udev device.
 * Fills structure with most usable udev device variables, properties
 * and sysattrs.
 */
static struct igt_device *igt_device_new_from_udev(struct udev_device *dev,
						   bool limit_attrs)
{
	struct igt_device *idev = igt_device_new();

	igt_assert(idev);
	idev->syspath = strdup_nullsafe(udev_device_get_syspath(dev));
	idev->subsystem = strdup_nullsafe(udev_device_get_subsystem(dev));
	idev->devnode = strdup_nullsafe(udev_device_get_devnode(dev));
	idev->sysname = strdup_nullsafe(udev_device_get_sysname(dev));

	if (idev->devnode && strstr(idev->devnode, "/dev/dri/card"))
		idev->drm_card = strdup(idev->devnode);
	else if (idev->devnode && strstr(idev->devnode, "/dev/dri/render"))
		idev->drm_render = strdup(idev->devnode);

	get_props(dev, idev);
	limit_attrs ? get_attrs_limited(dev, idev) :
		      get_attrs_all(dev, idev);

	if (is_pci_subsystem(idev)) {
		uint16_t vendor, device;

		if (!set_vendor_device(idev) || !set_pci_slot_name(idev)) {
			igt_device_free(idev);
			return NULL;
		}
		get_pci_vendor_device(idev, &vendor, &device);
		idev->codename = __pci_codename(vendor, device);
		idev->dev_type = __pci_devtype(vendor, device, idev->pci_slot_name);
		idev->driver = strdup_nullsafe(get_attr(idev, "driver"));
		igt_assert(idev->driver);
	}

	return idev;
}

/* Iterate over all igt_devices array and find one matched to
 * subsystem and syspath.
 */
static struct igt_device *igt_device_find(const char *subsystem,
					  const char *syspath)
{
	struct igt_device *dev;

	igt_list_for_each_entry(dev, &igt_devs.all, link)
	{
		if (strcmp(dev->subsystem, subsystem) == 0 &&
			strcmp(dev->syspath, syspath) == 0)
			return dev;
	}

	return NULL;
}

static bool is_vendor_matched(struct igt_device *dev, const char *vendor)
{
	const char *vendor_id;

	if (!dev->vendor || !vendor)
		return false;

	/* First we compare vendor id, like 8086 */
	if (!strcasecmp(dev->vendor, vendor))
		return true;

	/* Likely we have vendor string instead of id */
	vendor_id = get_pci_vendor_id_by_name(vendor);
	if (!vendor_id)
		return false;

	return !strcasecmp(dev->vendor, vendor_id);
}

static bool is_device_matched(struct igt_device *dev, const char *device)
{
	if (!dev->device || !device)
		return false;

	/* First we compare device id, like 1926 */
	if (!strcasecmp(dev->device, device))
		return true;

	/* Try "integrated" and "discrete" */
	if (dev->dev_type == DEVTYPE_INTEGRATED && !strcasecmp(device, STR_INTEGRATED))
		return true;
	else if (dev->dev_type == DEVTYPE_DISCRETE && !strcasecmp(device, STR_DISCRETE))
		return true;

	/* Try codename */
	return !strcasecmp(dev->codename, device);
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

static void
__copy_dev_to_card(struct igt_device *dev, struct igt_device_card *card)
{
	if (dev->subsystem != NULL)
		safe_strncpy(card->subsystem, dev->subsystem,
			     sizeof(card->subsystem));

	if (dev->drm_card != NULL)
		safe_strncpy(card->card, dev->drm_card, sizeof(card->card));

	if (dev->drm_render != NULL)
		safe_strncpy(card->render, dev->drm_render,
			     sizeof(card->render));

	if (dev->driver != NULL)
		safe_strncpy(card->driver, dev->driver,
			     sizeof(card->driver));

	if (dev->pci_slot_name != NULL)
		safe_strncpy(card->pci_slot_name, dev->pci_slot_name,
			     sizeof(card->pci_slot_name));

	if (dev->vendor != NULL)
		if (sscanf(dev->vendor, "%hx", &card->pci_vendor) != 1)
			card->pci_vendor = 0;

	if (dev->device != NULL)
		if (sscanf(dev->device, "%hx", &card->pci_device) != 1)
			card->pci_device = 0;
}

/*
 * Iterate over all igt_devices array and find first discrete/integrated card.
 * card->pci_slot_name will be updated only if a card is found.
 */
static bool __find_first_intel_card_by_driver_name(struct igt_device_card *card,
				bool want_discrete, const char *drv_name)
{
	struct igt_device *dev;
	int is_integrated;

	igt_assert(drv_name);
	memset(card, 0, sizeof(*card));

	igt_list_for_each_entry(dev, &igt_devs.all, link) {

		if (!is_pci_subsystem(dev) || strcmp(dev->driver, drv_name))
			continue;

		is_integrated = !strncmp(dev->pci_slot_name, INTEGRATED_I915_GPU_PCI_ID,
					 PCI_SLOT_NAME_SIZE);

		if (want_discrete && !is_integrated) {
			__copy_dev_to_card(dev, card);
			return true;
		} else if (!want_discrete && is_integrated) {
			__copy_dev_to_card(dev, card);
			return true;
		}
	}

	return false;
}

bool igt_device_find_first_i915_discrete_card(struct igt_device_card *card)
{
	igt_assert(card);

	return __find_first_intel_card_by_driver_name(card, true, "i915");
}

/**
 * igt_device_find_first_xe_discrete_card
 * @card: pointer to igt_device_card structure
 *
 * Iterate over all igt_devices array and find first xe discrete card.
 * card will be updated only if a device is found.
 *
 * Returns: true if device is found, false otherwise.
 */
bool igt_device_find_first_xe_discrete_card(struct igt_device_card *card)
{
	igt_assert(card);

	return __find_first_intel_card_by_driver_name(card, true, "xe");
}

bool igt_device_find_integrated_card(struct igt_device_card *card)
{
	igt_assert(card);

	return __find_first_intel_card_by_driver_name(card, false, "i915");
}

/**
 * igt_device_find_xe_integrated_card
 * @card: pointer to igt_device_card structure
 *
 * Iterate over all igt_devices array and find first xe integrated card.
 * card will be updated only if a device is found.
 *
 * Returns: true if device is found, false otherwise.
 */
bool igt_device_find_xe_integrated_card(struct igt_device_card *card)
{
	igt_assert(card);

	return __find_first_intel_card_by_driver_name(card, false, "xe");
}

static struct igt_device *igt_device_from_syspath(const char *syspath)
{
	struct igt_device *dev;

	igt_list_for_each_entry(dev, &igt_devs.all, link)
	{
		if (strcmp(dev->syspath, syspath) == 0)
			return dev;
	}

	return NULL;
}

static bool is_pcie_upstream_bridge(struct pci_dev *dev)
{
	struct pci_cap *pcie;
	uint8_t type;

	type = pci_read_byte(dev, PCI_HEADER_TYPE) & PCI_HEADER_TYPE_MASK;
	if (type != PCI_HEADER_TYPE_BRIDGE)
		return false;

	pcie = pci_find_cap(dev, PCI_CAP_ID_EXP, PCI_CAP_NORMAL);
	if (!pcie)
		return false;

	type = pci_read_word(dev, pcie->addr + PCI_EXP_FLAGS);
	type &= PCI_EXP_FLAGS_TYPE;
	type >>= ffs(PCI_EXP_FLAGS_TYPE) - 1;

	return type == PCI_EXP_TYPE_UPSTREAM;
}

#define RETRIES_GET_DEVICE 5

static struct igt_device *find_or_add_igt_device(struct udev *udev,
						 struct udev_device *dev,
						 bool limit_attrs)
{
	int retries = RETRIES_GET_DEVICE;
	const char *subsystem, *syspath;
	struct igt_device *idev;

	subsystem = udev_device_get_subsystem(dev);
	syspath = udev_device_get_syspath(dev);

	idev = igt_device_find(subsystem, syspath);
	while (!idev && retries--) {
		/*
		 * Don't care about previous parent_dev, it is tracked
		 * by the child node. There's very rare race when driver module
		 * is loaded or binded - in this moment getting parent device
		 * may finish with incomplete properties. Unfortunately even
		 * if we notice this (missing PCI_ID or PCI_SLOT_NAME)
		 * consecutive calling udev_device_get_parent() will return
		 * stale (cached parent) device. We don't want this so
		 * only udev_device_new*() will scan sys directory and
		 * return fresh udev device.
		 */
		dev = udev_device_new_from_syspath(udev, syspath);
		idev = igt_device_new_from_udev(dev, limit_attrs);
		udev_device_unref(dev);

		if (idev)
			igt_list_add_tail(&idev->link, &igt_devs.all);
		else
			usleep(100000); /* arbitrary, 100ms should be enough */
	}

	return idev;
}

static struct udev_device *get_pcie_upstream_bridge(struct udev *udev,
						    struct udev_device *dev,
						    struct pci_access *pacc)
{
	igt_assert(pacc);

	for (dev = udev_device_get_parent(dev); dev; dev = udev_device_get_parent(dev)) {
		struct pci_filter filter;
		struct pci_dev *pci_dev;
		const char *slot;

		slot = udev_device_get_property_value(dev, "PCI_SLOT_NAME");
		if (igt_debug_on(!slot))
			continue;

		pci_filter_init(pacc, &filter);
		if (igt_debug_on(pci_filter_parse_slot(&filter, (char *)slot)))
			continue;

		pci_dev = pci_get_dev(pacc, filter.domain, filter.bus, filter.slot, filter.func);
		if (igt_debug_on(!pci_dev))
			continue;

		if (is_pcie_upstream_bridge(pci_dev))
			break;
	}

	return dev;
}

/*
 * For each drm igt_device add or update its parent igt_device to the array.
 * As card/render drm devices mostly have same parent (vkms is an exception)
 * link to it and update corresponding drm_card / drm_render fields.
 *
 * If collecting all attributes and the parent is a discrete GPU then also
 * add or update its bridge's upstream port.
 */
static void update_or_add_parent(struct udev *udev,
				 struct udev_device *dev,
				 struct igt_device *idev,
				 struct pci_access *pacc,
				 bool limit_attrs)
{
	struct igt_device *parent_idev, *bridge_idev;
	struct udev_device *parent_dev, *bridge_dev;
	const char *devname;

	/*
	 * Get parent for drm node. It caches parent in udev device
	 * and will be destroyed along with the node.
	 */
	parent_dev = udev_device_get_parent(dev);
	igt_assert(parent_dev);

	parent_idev = find_or_add_igt_device(udev, parent_dev, limit_attrs);
	igt_assert(parent_idev);

	devname = udev_device_get_devnode(dev);
	if (devname != NULL && strstr(devname, "/dev/dri/card"))
		parent_idev->drm_card = strdup(devname);
	else if (devname != NULL && strstr(devname, "/dev/dri/render"))
		parent_idev->drm_render = strdup(devname);

	idev->parent = parent_idev;

	if (!pacc || parent_idev->dev_type != DEVTYPE_DISCRETE)
		return;

	bridge_dev = get_pcie_upstream_bridge(udev, parent_dev, pacc);
	if (!bridge_dev)
		return;

	bridge_idev = find_or_add_igt_device(udev, bridge_dev, limit_attrs);
	igt_assert(bridge_idev);

	/* override DEVTYPE_INTEGRATED so link attributes won't be omitted */
	bridge_idev->dev_type = DEVTYPE_ALL;
	/* free numeric codename before overwriting with GPU codename */
	free(bridge_idev->codename);
	bridge_idev->codename = strdup(parent_idev->codename);

	bridge_idev->pci_gpu = strdup(parent_idev->pci_slot_name);
	parent_idev->parent = bridge_idev;
}

static struct igt_device *duplicate_device(struct igt_device *dev) {
	struct igt_device *dup = malloc(sizeof(*dup));
	memcpy(dup, dev, sizeof(*dev));
	dup->link.prev = NULL;
	dup->link.next = NULL;
	return dup;
}

static int devs_compare(const void *a, const void *b)
{
	struct igt_device *dev1, *dev2;
	unsigned int len1, len2;
	int ret;

	dev1 = *(struct igt_device **) a;
	dev2 = *(struct igt_device **) b;
	ret = strcmp(dev1->subsystem, dev2->subsystem);
	if (ret)
		return ret;

	len1 = strlen(dev1->syspath);
	len2 = strlen(dev2->syspath);

	if (len1 != len2 && !strncmp(dev1->syspath, dev2->syspath, min(len1, len2)))
		return len2 - len1;

	return strcmp(dev1->syspath, dev2->syspath);
}

static void sort_all_devices(void)
{
	struct igt_device *dev, *tmp;
	int len = igt_list_length(&igt_devs.all);
	struct igt_device **devs = malloc(len * sizeof(*dev));

	int i = 0;
	igt_list_for_each_entry_safe(dev, tmp, &igt_devs.all, link) {
		devs[i] = dev;
		igt_assert(i++ < len);
		igt_list_del(&dev->link);
	}

	qsort(devs, len, sizeof(*devs), devs_compare);

	for (i = 0; i < len; ++i) {
		igt_list_add_tail(&devs[i]->link, &igt_devs.all);
	}

	free(devs);
}

static void index_pci_devices(void)
{
	struct igt_device *dev;

	igt_list_for_each_entry(dev, &igt_devs.all, link) {
		struct igt_device *dev2;
		int index = 0;

		if (!is_pci_subsystem(dev))
			continue;

		igt_list_for_each_entry(dev2, &igt_devs.all, link) {
			if (!is_pci_subsystem(dev2))
				continue;

			if (dev2 == dev)
				break;

			if (!strcasecmp(dev->vendor, dev2->vendor) &&
			    !strcasecmp(dev->device, dev2->device))
				index++;
		}

		dev->gpu_index = index;
	}
}

/* Core scanning function.
 *
 * All scanned devices are kept inside igt_devs.all pointer array.
 * Each added device is igt_device structure, which contrary to udev device
 * has properties / sysattrs stored inside hash table instead of list.
 *
 * Function iterates over devices on 'drm' subsystem. For each drm device
 * its parent is taken (bus device) and stored inside same array.
 * Function sorts all found devices to keep same order of bus devices
 * for providing predictable search.
 */
static void scan_drm_devices(bool limit_attrs)
{
	struct udev *udev;
	struct udev_enumerate *enumerate;
	struct udev_list_entry *devices, *dev_list_entry;
	struct pci_access *pacc = NULL;
	struct igt_device *dev;
	int ret;

	udev = udev_new();
	igt_assert(udev);

	enumerate = udev_enumerate_new(udev);
	igt_assert(enumerate);

	DBG("Scanning drm subsystem\n");
	ret = udev_enumerate_add_match_subsystem(enumerate, "drm");
	igt_assert(!ret);

	ret = udev_enumerate_add_match_property(enumerate, "DEVNAME", "/dev/dri/*");
	igt_assert(!ret);

	ret = udev_enumerate_scan_devices(enumerate);
	igt_assert(!ret);

	devices = udev_enumerate_get_list_entry(enumerate);
	if (!devices)
		return;

	/* prepare for upstream bridge port scan if called from lsgpu */
	if (!limit_attrs) {
		pacc = pci_alloc();
		pci_init(pacc);
	}

	udev_list_entry_foreach(dev_list_entry, devices) {
		const char *path;
		struct udev_device *udev_dev;
		struct igt_device *idev;

		path = udev_list_entry_get_name(dev_list_entry);
		udev_dev = udev_device_new_from_syspath(udev, path);
		idev = igt_device_new_from_udev(udev_dev, limit_attrs);
		igt_list_add_tail(&idev->link, &igt_devs.all);
		update_or_add_parent(udev, udev_dev, idev, pacc, limit_attrs);

		udev_device_unref(udev_dev);
	}
	if (pacc)
		pci_cleanup(pacc);
	udev_enumerate_unref(enumerate);
	udev_unref(udev);

	sort_all_devices();
	index_pci_devices();

	igt_list_for_each_entry(dev, &igt_devs.all, link) {
		struct igt_device *dev_dup = duplicate_device(dev);
		igt_list_add_tail(&dev_dup->link, &igt_devs.filtered);
	}
}

static void free_key_value(struct igt_map_entry *entry)
{
	free((char *)entry->key);
	free(entry->data);
}

static void igt_device_free(struct igt_device *dev)
{
	free(dev->codename);
	free(dev->devnode);
	free(dev->subsystem);
	free(dev->syspath);
	free(dev->drm_card);
	free(dev->drm_render);
	free(dev->vendor);
	free(dev->device);
	free(dev->driver);
	free(dev->pci_slot_name);
	free(dev->pci_gpu);
	igt_map_destroy(dev->attrs_map, free_key_value);
	igt_map_destroy(dev->props_map, free_key_value);
}

void igt_devices_free(void)
{
	struct igt_device *dev, *tmp;

	igt_list_for_each_entry_safe(dev, tmp, &igt_devs.filtered, link) {
		igt_list_del(&dev->link);
		free(dev);
	}

	igt_list_for_each_entry_safe(dev, tmp, &igt_devs.all, link) {
		igt_list_del(&dev->link);
		igt_device_free(dev);
		free(dev);
	}
	igt_devs.devs_scanned = false;
}

/**
 * igt_devices_scan
 * @force: enforce scanning devices
 *
 * Function scans udev in search of gpu devices.
 */

static void __igt_devices_scan(bool limit_attrs)
{
	if (igt_devs.devs_scanned)
		igt_devices_free();

	prepare_scan();
	scan_drm_devices(limit_attrs);

	igt_devs.devs_scanned = true;
}

void igt_devices_scan(void)
{
	__igt_devices_scan(true);
}

void igt_devices_scan_all_attrs(void)
{
	__igt_devices_scan(false);
}

static inline void _pr_simple(const char *k, const char *v)
{
	printf("    %-16s: %s\n", k, v);
}

static inline void _pr_simple2(const char *k, const char *v1, const char *v2)
{
	printf("    %-16s: %s:%s\n", k, v1, v2);
}

static bool __check_empty(struct igt_list_head *view)
{
	if (!view)
		return true;

	if (igt_list_empty(view)) {
		printf("No GPU devices found\n");
		return true;
	}

	return false;
}

static void
igt_devs_print_simple(struct igt_list_head *view,
		      const struct igt_devices_print_format *fmt)
{
	struct igt_device *dev;

	if (__check_empty(view))
		return;

	igt_list_for_each_entry(dev, view, link) {
		printf("sys:%s\n", dev->syspath);
		if (dev->subsystem)
			_pr_simple("subsystem", dev->subsystem);
		if (dev->drm_card)
			_pr_simple("drm card", dev->drm_card);
		if (dev->drm_render)
			_pr_simple("drm render", dev->drm_render);
		if (is_drm_subsystem(dev)) {
			_pr_simple2("parent", "sys",
				   dev->parent->syspath);
		} else {
			if (is_pci_subsystem(dev)) {
				_pr_simple("vendor", dev->vendor);
				_pr_simple("device", dev->device);
				if (dev->pci_gpu)
					_pr_simple("GPU device", dev->pci_gpu);
				_pr_simple("codename", dev->codename);
				if (dev->parent && dev->parent->pci_slot_name)
					_pr_simple("upstream port", dev->parent->pci_slot_name);
			}
		}
		printf("\n");
	}
}

static struct igt_device *
__find_pci(struct igt_list_head *view, const char *drm)
{
	struct igt_device *dev;

	igt_list_for_each_entry(dev, view, link) {
		if (!is_pci_subsystem(dev) || !dev->drm_card)
			continue;

		if (!strcmp(dev->drm_card, drm))
			return dev;
	}

	return NULL;
}

static void __print_filter(char *buf, int len,
			   const struct igt_devices_print_format *fmt,
			   struct igt_device *dev,
			   bool render)
{
	int ret;

	switch (fmt->option) {
	case IGT_PRINT_DRM:
		ret = snprintf(buf, len, "drm:%s",
			       render ? dev->drm_render : dev->drm_card);
		igt_assert(ret < len);
		break;
	case IGT_PRINT_SYSFS:
		ret = snprintf(buf, len, "sys:%s", dev->syspath);
		igt_assert(ret < len);
		break;
	case IGT_PRINT_PCI:
		if (!render) {
			ret = snprintf(buf, len,
				       "pci:vendor=%s,device=%s,card=%d",
				       dev->vendor, dev->device,
				       dev->gpu_index);
			igt_assert(ret < len);
		}
		break;
	};
}

#define VENDOR_SIZE 30
static void
igt_devs_print_user(struct igt_list_head *view,
		    const struct igt_devices_print_format *fmt)
{
	struct igt_device *dev;

	if (__check_empty(view))
		return;

	igt_list_for_each_entry(dev, view, link) {
		unsigned int i, num_children;
		struct igt_device *pci_dev;
		struct igt_device *dev2;
		char filter[256];
		char *drm_name;

		if (!is_drm_subsystem(dev))
			continue;
		if (!dev->drm_card || dev->drm_render)
			continue;

		drm_name = strrchr(dev->drm_card, '/');
		if (!drm_name || !*++drm_name)
			continue;

		pci_dev = __find_pci(view, dev->drm_card);

		if (fmt->option == IGT_PRINT_PCI && !pci_dev)
			continue;

		if (pci_dev) {
			uint16_t vendor, device;
			char *devname;

			get_pci_vendor_device(pci_dev, &vendor, &device);
			if (fmt->codename)
				devname = __pci_codename(vendor, device);
			else
				devname = __pci_pretty_name(vendor, device, fmt->numeric);

			__print_filter(filter, sizeof(filter), fmt, pci_dev,
				       false);

			printf("%-24s %-*s    %s\n",
			       drm_name, VENDOR_SIZE, devname, filter);

			free(devname);
		} else {
			__print_filter(filter, sizeof(filter), fmt, dev, false);
			printf("%-24s             %s\n", drm_name, filter);
		}

		num_children = 0;
		igt_list_for_each_entry(dev2, view, link) {
			if (!is_drm_subsystem(dev2) || !dev2->drm_render)
				continue;
			if (strcmp(dev2->parent->syspath, dev->parent->syspath))
				continue;

			num_children++;
		}

		i = 0;
		igt_list_for_each_entry(dev2, view, link) {
			if (!is_drm_subsystem(dev2) || !dev2->drm_render)
				continue;
			if (strcmp(dev2->parent->syspath, dev->parent->syspath))
				continue;

			drm_name = strrchr(dev2->drm_render, '/');
			if (!drm_name || !*++drm_name)
				continue;

			printf("%s%-22s",
				(++i == num_children) ? "└─" : "├─", drm_name);
			if (fmt->option != IGT_PRINT_PCI) {
				__print_filter(filter, sizeof(filter), fmt,
					       dev2, true);
				printf("%-*s     %s\n", VENDOR_SIZE, "", filter);
			} else {
				printf("\n");
			}
		}
	}
}

static void
igt_devs_print_detail(struct igt_list_head *view,
		      const struct igt_devices_print_format *fmt)

{
	struct igt_device *dev;

	if (__check_empty(view))
		return;

	igt_list_for_each_entry(dev, view, link) {
		printf("========== %s:%s ==========\n",
		       dev->subsystem, dev->syspath);
		if (!is_drm_subsystem(dev)) {
			if (dev->drm_card)
				_print_key_value("card device", dev->drm_card);
			if (dev->drm_render)
				_print_key_value("render device", dev->drm_render);
			if (dev->pci_gpu)
				_print_key_value("GPU device", dev->pci_gpu);
			_print_key_value("codename", dev->codename);
		}

		/* omit fake link bandwidth attributes if a discrete card */
		dump_props_and_attrs(dev, dev->dev_type == DEVTYPE_DISCRETE);
	}
}

static struct print_func {
	void (*prn)(struct igt_list_head *view,
		    const struct igt_devices_print_format *);
} print_functions[] = {
	[IGT_PRINT_SIMPLE] = { .prn = igt_devs_print_simple },
	[IGT_PRINT_DETAIL] = { .prn = igt_devs_print_detail },
	[IGT_PRINT_USER] = { .prn = igt_devs_print_user },
};

/**
 * igt_devices_print
 * @fmt: Print format as specified by struct igt_devices_print_format
 *
 * Function can be used by external tool to print device array in simple
 * or detailed form. This function is added here to avoid exposing
 * internal implementation data structures.
 */
void igt_devices_print(const struct igt_devices_print_format *fmt)
{
	print_functions[fmt->type].prn(&igt_devs.filtered, fmt);
}

/**
 * igt_devices_print_vendors
 *
 * Print pci id -> vendor mappings. Vendor names printed by this function
 * can be used for filters like pci which allows passing vendor - like
 * vendor id (8086) or as a string (Intel).
 */
void igt_devices_print_vendors(void)
{
	printf("Recognized vendors:\n");
	printf("%-8s %-16s\n", "PCI ID", "vendor");
	for (typeof(*pci_vendor_mapping) *vm = pci_vendor_mapping; vm->name; vm++) {
		printf("%-8s %-16s\n", vm->vendor_id, vm->name);
	}
}

struct filter;

/* ------------------------------------------------------------------------- */
struct filter_class {
	struct igt_list_head *(*filter_function)(const struct filter_class *fcls,
						 const struct filter *filter);
	bool (*is_valid)(const struct filter_class *fcls,
			 const struct filter *filter);
	const char *name;
	const char *help;
	const char *detail;
};

#define FILTER_NAME_LEN 32
#define FILTER_DATA_LEN 256

struct filter {
	struct filter_class *class;

	char raw_data[FILTER_DATA_LEN];

	struct {
		char *vendor;
		char *device;
		char *card;
		char *slot;
		char *drm;
		char *driver;
		char *pf;
		char *vf;
		char *subsystem;
	} data;
};

static void fill_filter_data(struct filter *filter, const char *key, const char *value)
{
	if (key == NULL || value == NULL) {
		return;
	}

#define __fill_key(name) if (strcmp(key, #name) == 0) \
	filter->data.name = strdup(value)
	__fill_key(vendor);
	__fill_key(device);
	__fill_key(card);
	__fill_key(slot);
	__fill_key(drm);
	__fill_key(driver);
	__fill_key(pf);
	__fill_key(vf);
	__fill_key(subsystem);
#undef __fill_key

}

static void split_filter_data(struct filter *filter)
{
	char *property, *key, *data, *dup;

	dup = strdup(filter->raw_data);
	data = dup;

	while ((property = strsep(&data, ","))) {
		key = strsep(&property, "=");
		fill_filter_data(filter, key, property);
	}

	free(dup);
}

static struct filter_class *get_filter_class(const char *class_name, const struct filter *filter);

static bool parse_filter(const char *fstr, struct filter *filter)
{
	char class_name[32];
	if (!fstr || !filter)
		return false;

	memset(filter, 0, sizeof(*filter));

	if (sscanf(fstr, "%31[^:]:%255s", class_name, filter->raw_data) >= 1) {
		filter->class = get_filter_class(class_name, filter);
		split_filter_data(filter);
		return true;
	}

	return false;
}

/* Filter which matches subsystem:/sys/... path.
 * Used as first filter in chain.
 */
static struct igt_list_head *filter_sys(const struct filter_class *fcls,
					const struct filter *filter)
{
	struct igt_device *dev;
	(void) fcls;

	DBG("filter sys\n");
	if (!strlen(filter->raw_data))
		return &igt_devs.filtered;

	dev = igt_device_from_syspath(filter->raw_data);
	if (dev) {
		struct igt_device *dup = duplicate_device(dev);
		igt_list_add_tail(&dup->link, &igt_devs.filtered);
	}

	return &igt_devs.filtered;
}

/* Find drm device using direct path to /dev/dri/.
 * It extends filter_sys to allow using drm:/dev/dri/cardX and
 * drm:/dev/dri/renderDX filter syntax.
 */
static struct igt_list_head *filter_drm(const struct filter_class *fcls,
					const struct filter *filter)
{
	struct igt_device *dev;
	(void) fcls;

	DBG("filter drm\n");
	if (!strlen(filter->raw_data))
		return &igt_devs.filtered;

	igt_list_for_each_entry(dev, &igt_devs.all, link) {
		if (!is_drm_subsystem(dev))
			continue;

		if (strequal(dev->syspath, filter->raw_data) ||
			strequal(dev->drm_card, filter->raw_data) ||
			strequal(dev->drm_render, filter->raw_data)) {
			struct igt_device *dup = duplicate_device(dev);
			igt_list_add_tail(&dup->link, &igt_devs.filtered);
			break;
		}
	}


	return &igt_devs.filtered;
}

/* Find appropriate pci device matching vendor/device/card filter arguments.
 */
static struct igt_list_head *filter_pci(const struct filter_class *fcls,
					const struct filter *filter)
{
	struct igt_device *dev;
	int card = -1;
	(void) fcls;

	DBG("filter pci\n");

	if (filter->data.slot && (filter->data.vendor || filter->data.device || filter->data.card)) {
		fprintf(stderr, "Slot parameter can not be used with other parameters\n");
		exit(EXIT_FAILURE);
	}

	if (filter->data.card) {
		sscanf(filter->data.card, "%d", &card);
		if (card < 0) {
			return &igt_devs.filtered;
		}
	} else {
		card = 0;
	}

	igt_list_for_each_entry(dev, &igt_devs.all, link) {
		if (!is_pci_subsystem(dev))
			continue;

		/* Skip if 'slot' doesn't match */
		if (filter->data.slot && !strequal(filter->data.slot, dev->pci_slot_name))
			continue;

		/* Skip if 'vendor' doesn't match (hex or name) */
		if (filter->data.vendor && !is_vendor_matched(dev, filter->data.vendor))
			continue;

		/* Skip if 'device' doesn't match */
		if (filter->data.device && !is_device_matched(dev, filter->data.device))
			continue;

		/* We get n-th card */
		if (!card) {
			struct igt_device *dup = duplicate_device(dev);
			igt_list_add_tail(&dup->link, &igt_devs.filtered);
			break;
		}
		card--;
	}

	DBG("Filter pci filtered size: %d\n", igt_list_length(&igt_devs.filtered));

	return &igt_devs.filtered;
}

static bool is_pf(struct igt_device *dev)
{
	if (get_attr(dev, "sriov_numvfs") == NULL)
		return false;

	return true;
}

static bool is_vf(struct igt_device *dev)
{
	if (get_attr(dev, "physfn") == NULL)
		return false;

	return true;
}

/*
 * Find appropriate pci device matching vendor/device/card/pf/vf filter arguments.
 */
static struct igt_list_head *filter_sriov(const struct filter_class *fcls,
					const struct filter *filter)
{
	struct igt_device *dev, *dup;
	int card = -1, pf = -1, vf = -1;
	char *pf_pci_slot_name = NULL;
	(void) fcls;

	DBG("filter sriov\n");

	if (filter->data.card) {
		sscanf(filter->data.card, "%d", &card);
		if (card < 0) {
			return &igt_devs.filtered;
		}
	} else {
		card = 0;
	}

	if (filter->data.pf) {
		sscanf(filter->data.pf, "%d", &pf);
		if (pf < 0) {
			return &igt_devs.filtered;
		}
	} else {
		pf = 0;
	}

	if (filter->data.vf) {
		sscanf(filter->data.vf, "%d", &vf);
		if (vf < 0) {
			return &igt_devs.filtered;
		}
	}

	igt_list_for_each_entry(dev, &igt_devs.all, link) {
		if (!is_pci_subsystem(dev))
			continue;

		/* Skip if 'vendor' doesn't match (hex or name) */
		if (filter->data.vendor && !is_vendor_matched(dev, filter->data.vendor))
			continue;

		/* Skip if 'device' doesn't match */
		if (filter->data.device && !is_device_matched(dev, filter->data.device))
			continue;

		/* We get n-th card */
		if (!card) {
			if (!pf) {
				if (is_pf(dev))
					pf_pci_slot_name = dev->pci_slot_name;

				/* vf parameter was not passed, get pf */
				if (vf < 0) {
					if (!is_pf(dev))
						continue;

					dup = duplicate_device(dev);
					igt_list_add_tail(&dup->link, &igt_devs.filtered);
					break;
				} else {
					/* Skip if vf is not associated with defined pf */
					if (!strequal(get_attr(dev, "physfn"), pf_pci_slot_name))
						continue;

					if (!vf) {
						if (!is_vf(dev))
							continue;

						dup = duplicate_device(dev);
						igt_list_add_tail(&dup->link, &igt_devs.filtered);
						break;
					}
					if (is_vf(dev)) {
						vf--;
						continue;
					}
				}
			}
			if (is_pf(dev)) {
				pf--;
				continue;
			}
		}
		card--;
	}

	return &igt_devs.filtered;
}

/*
 * Find appropriate gpu device through matching driver, device type and
 * card filter arguments.
 */
static struct igt_list_head *filter_device(const struct filter_class *fcls,
					   const struct filter *filter)
{
	struct igt_device *dev;
	bool allcards = false;
	int card = 0;
	(void)fcls;

	DBG("filter device\n");
	if (filter->data.card) {
		char crdop[5] = {0};

		if (sscanf(filter->data.card, "%d", &card) == 1) {
			if (card < 0)
				return &igt_devs.filtered;
		} else {
			card = 0;
			if (sscanf(filter->data.card, "%4s", crdop) == 1) {
				if (!strcmp(crdop, "all"))
					allcards = true;
				else
					return &igt_devs.filtered;
			} else {
				return &igt_devs.filtered;
			}
		}
	} else {
		card = 0;
	}

	igt_list_for_each_entry(dev, &igt_devs.all, link) {
		/* Skip if 'driver' doesn't match */
		if (filter->data.driver && !strequal(filter->data.driver, dev->driver))
			continue;

		/* Skip if 'device' doesn't match */
		if (filter->data.device && !is_device_matched(dev, filter->data.device))
			continue;

		/* Skip if 'subsystem' doesn't match */
		if (filter->data.subsystem && strcmp(filter->data.subsystem, "all")) {
			if (strcmp(filter->data.subsystem, get_prop_subsystem(dev)))
				continue;
		}

		/* We get n-th card */
		if (!allcards && !card) {
			struct igt_device *dup = duplicate_device(dev);

			igt_list_add_tail(&dup->link, &igt_devs.filtered);
			break;
		} else if (!allcards) {
			card--;
		}
		/* Include all the cards */
		else if (allcards) {
			struct igt_device *dup = duplicate_device(dev);

			igt_list_add(&dup->link, &igt_devs.filtered);
		}
	}

	DBG("Filter device filtered size: %d\n", igt_list_length(&igt_devs.filtered));

	return &igt_devs.filtered;
}

static bool sys_path_valid(const struct filter_class *fcls,
			   const struct filter *filter)
{
	struct stat st;

	if (stat(filter->raw_data, &st)) {
		igt_warn("sys_path_valid: syspath [%s], err: %s\n",
			 filter->raw_data, strerror(errno));
		return false;
	}

	return true;
}


static struct filter_class filter_definition_list[] = {
	{
		.name = "sys",
		.is_valid = sys_path_valid,
		.filter_function = filter_sys,
		.help = "sys:/sys/devices/pci0000:00/0000:00:02.0",
		.detail = "find device by its sysfs path\n",
	},
	{
		.name = "drm",
		.filter_function = filter_drm,
		.help = "drm:/dev/dri/* path",
		.detail = "find drm device by /dev/dri/* node\n",
	},
	{
		.name = "pci",
		.filter_function = filter_pci,
		.help = "pci:[vendor=%04x/name][,device=%04x][,card=%d] | [slot=%04x:%02x:%02x.%x]",
		.detail = "vendor is hex number or vendor name\n",
	},
	{
		.name = "sriov",
		.filter_function = filter_sriov,
		.help = "sriov:[vendor=%04x/name][,device=%04x][,card=%d][,pf=%d][,vf=%d]",
		.detail = "find pf or vf\n",
	},
	{
		.name = "device",
		.filter_function = filter_device,
		.help =
		"device:[driver=name][,subsystem=all|<subsystem>][,device=type][,card=%d|all]",
		.detail = "find device by driver name, subsystem, device type and card number\n",
	},
	{
		.name = NULL,
	},
};

static struct filter_class *get_filter_class(const char *class_name, const struct filter *filter)
{
	struct filter_class *fcls = NULL;
	int i = 0;

	while ((fcls = &filter_definition_list[i++])->name != NULL) {
		if (strcmp(class_name, fcls->name) == 0)
			return fcls;
	}

	return NULL;
}

/**
 * @igt_device_print_filter_types
 *
 * Print all filters syntax for device selection.
 */
void igt_device_print_filter_types(void)
{
	const struct filter_class *filter = NULL;
	int i = 0;

	printf("Filter types:\n---\n");
	printf("%-12s  %s\n---\n", "filter", "syntax");

	while ((filter = &filter_definition_list[i++])->name != NULL) {
		printf("%-12s  %s\n", filter->name, filter->help);
		printf("%-12s  %s\n", "", filter->detail);
	}
}

struct device_filter {
	char filter[NAME_MAX];
	struct igt_list_head link;
};

static IGT_LIST_HEAD(device_filters);

/**
 * igt_device_filter_count
 *
 * Returns number of filters collected in the filter list.
 */
int igt_device_filter_count(void)
{
	return igt_list_length(&device_filters);
}

/* Check does filter is valid. It checks:
 * 1. /sys/... path first
 * 2. filter name from filter definition
 */
static bool is_filter_valid(const char *fstr)
{
	struct filter filter;
	int ret;

	ret = parse_filter(fstr, &filter);
	if (!ret)
		return false;

	if (filter.class == NULL) {
		igt_warn("No filter class matching [%s]\n", fstr);
		return false;
	}

	if (filter.class->is_valid != NULL && !filter.class->is_valid(filter.class, &filter))
	{
		igt_warn("Filter not valid [%s:%s]\n", filter.class->name, filter.raw_data);
		return false;
	}

	return true;
}

#define MAX_PCI_CARDS 64

/**
 * igt_device_filter_add
 * @filters: filter(s) to be stored in filter array
 *
 * Function allows passing single or more filters within one string. This is
 * for CI when it can extract filter from environment variable (and it must
 * be single string). So if @filter contains semicolon ';' it treats
 * each part as separate filter and adds to the filter array.
 *
 * Returns number of filters added to filter array. Can be greater than
 * 1 if @filters contains more than one filter separated by semicolon.
 */
int igt_device_filter_add(const char *filters)
{
	char *dup, *dup_orig, *filter;
	int count = 0;

	dup = strdup(filters);
	dup_orig = dup;

	while ((filter = strsep(&dup, ";"))) {
		bool is_valid = is_filter_valid(filter);
		struct device_filter *df;
		char *multi;

		igt_warn_on(!is_valid);
		if (!is_valid)
			continue;

		if (!strncmp(filter, "sriov:", 6)) {
			multi = NULL;
		} else {
			multi = strstr(filter, "card=all");
			if (!multi)
				multi = strstr(filter, "card=*");
		}

		if (!multi) {
			df = malloc(sizeof(*df));
			strncpy(df->filter, filter, sizeof(df->filter)-1);
			igt_list_add_tail(&df->link, &device_filters);
			count++;
		} else {
			multi[5] = 0;
			for (int i = 0; i < MAX_PCI_CARDS; ++i) {
				df = malloc(sizeof(*df));
				snprintf(df->filter, sizeof(df->filter)-1, "%s%d", filter, i);
				if (i) { /* add at least card=0 */
					struct igt_device_card card;

					if (!igt_device_card_match(df->filter, &card)) {
						free(df);
						break;
					}
				}

				igt_list_add_tail(&df->link, &device_filters);
				count++;
			}
		}
	}

	free(dup_orig);

	return count;
}

/**
 * igt_device_filter_free_all
 *
 * Free all filters within array.
 */
void igt_device_filter_free_all(void)
{
	struct device_filter *filter, *tmp;

	igt_list_for_each_entry_safe(filter, tmp, &device_filters, link) {
		igt_list_del(&filter->link);
		free(filter);
	}
}

/**
 * igt_device_filter_get
 * @num: Number of filter from filter array
 *
 * Returns filter string or NULL if @num is out of range of filter array.
 */
const char *igt_device_filter_get(int num)
{
	struct device_filter *filter;
	int i = 0;

	if (num < 0)
		return NULL;


	igt_list_for_each_entry(filter, &device_filters, link) {
		if (i == num)
			return filter->filter;
		i++;
	}


	return NULL;
}

/**
 * igt_device_filter_pci
 *
 * Filter devices to PCI only.
 *
 * Returns PCI devices count.
 */
int igt_device_filter_pci(void)
{
	int count = 0;
	struct igt_device *dev, *tmp;

	igt_list_for_each_entry_safe(dev, tmp, &igt_devs.filtered, link)
		if (strcmp(dev->subsystem, "pci") != 0) {
			igt_list_del(&dev->link);
			free(dev);
		} else {
			count++;
		}

	return count;
}

static bool igt_device_filter_apply(const char *fstr)
{
	struct igt_device *dev, *tmp;
	struct filter filter;
	bool ret;

	if (!fstr)
		return false;

	ret = parse_filter(fstr, &filter);
	if (!ret) {
		igt_warn("Can't split filter [%s]\n", fstr);
		return false;
	}

	/* Clean the filtered list */
	igt_list_for_each_entry_safe(dev, tmp, &igt_devs.filtered, link) {
		igt_list_del(&dev->link);
		free(dev);
	}

	/* If filter.data contains "/sys" use direct path instead
	 * contextual filter.
	 */

	if (!filter.class) {
		igt_warn("No filter class matching [%s]\n", fstr);
		return false;
	}
	filter.class->filter_function(filter.class, &filter);

	return true;
}


static bool __igt_device_card_match(const char *filter,
			struct igt_device_card *card, bool request_pci_ss)
{
	struct igt_device *dev = NULL;

	if (!card)
		return false;
	memset(card, 0, sizeof(*card));

	/*
	 * Scan devices in case the user hasn't yet,
	 * but leave a decision on forced rescan on the user side.
	 */
	igt_devices_scan();

	if (igt_device_filter_apply(filter) == false)
		return false;

	if (igt_list_empty(&igt_devs.filtered))
		return false;

	/* We take first one if more than one card matches filter */
	dev = igt_list_first_entry(&igt_devs.filtered, dev, link);
	if (request_pci_ss && !is_pci_subsystem(dev) && dev->parent
		&& is_pci_subsystem(dev->parent))
		__copy_dev_to_card(dev->parent, card);
	else
		__copy_dev_to_card(dev, card);
	return true;
}

/**
 * igt_device_card_match
 * @filter: filter string
 * @card: pointer to igt_device_card struct
 *
 * Function applies filter to match device from device array.
 *
 * Returns:
 * false - no card pointer was passed or card wasn't matched,
 * true - card matched and returned.
 */
bool igt_device_card_match(const char *filter, struct igt_device_card *card)
{
       return __igt_device_card_match(filter, card, false);
}

/**
 * igt_device_card_match_pci
 * @filter: filter string
 * @card: pointer to igt_device_card struct
 *
 * Function applies filter to match device from device array.
 * Populate associated pci subsystem data if available.
 *
 * Returns:
 * false - no card pointer was passed or card wasn't matched,
 * true - card matched and returned.
 */
bool igt_device_card_match_pci(const char *filter,
			struct igt_device_card *card)
{
       return __igt_device_card_match(filter, card, true);
}

bool igt_device_find_card_by_sysname(const char *sysname,
				     struct igt_device_card *card)
{
	struct igt_device *dev;

	igt_assert(card);
	igt_assert(sysname);

	memset(card, 0, sizeof(*card));

	igt_list_for_each_entry(dev, &igt_devs.all, link) {
		if (strcmp(dev->sysname, sysname) == 0) {
			__copy_dev_to_card(dev, card);
			return true;
		}
	}

	return false;
}

/**
 * igt_device_card_match_all
 * @filter: filter string.
 * @card: double pointer to igt_device_card structure, containing
 * an array of igt_device_card structures upon successful return.
 *
 * Function applies filter to match device from device array.
 *
 * Returns: the number of cards found.
 *
 * Note: The caller is responsible for freeing the memory which is
 * dynamically allocated for the array of igt_device_card structures
 * upon successful return.
 */
int igt_device_card_match_all(const char *filter, struct igt_device_card **card)
{
	struct igt_device *dev = NULL;
	struct igt_device_card *crd = NULL;
	int count = 0;

	igt_devices_scan();

	if (igt_device_filter_apply(filter) == false)
		return 0;

	if (igt_list_empty(&igt_devs.filtered))
		return 0;

	igt_list_for_each_entry(dev, &igt_devs.filtered, link) {
		count++;
	}

	crd = calloc(count, sizeof(struct igt_device_card));
	if (!crd)
		return 0;

	count = 0;

	igt_list_for_each_entry(dev, &igt_devs.filtered, link) {
		__copy_dev_to_card(dev, crd + count++);
	}

	if (count)
		*card = crd;

	return count;
}

/**
 * igt_device_get_pretty_name
 * @card: pointer to igt_device_card struct
 *
 * For card function returns allocated string having pretty name
 * or vendor:device as hex if no backend pretty-resolver is implemented.
 *
 * Returns: newly allocated string.
 */
char *igt_device_get_pretty_name(struct igt_device_card *card, bool numeric)
{
	char *devname;

	igt_assert(card);

	if (strlen(card->pci_slot_name))
		devname = __pci_pretty_name(card->pci_vendor, card->pci_device,
					    numeric);
	else
		devname = strdup(card->subsystem);

	return devname;
}

/**
 * igt_open_card:
 * @card: pointer to igt_device_card structure
 *
 * Open /dev/dri/cardX device represented by igt_device_card structure.
 * Requires filled @card argument (see igt_device_card_match() function).
 *
 * An open DRM fd or -1 on error
 */
int igt_open_card(struct igt_device_card *card)
{
	if (!card || !strlen(card->card))
		return -1;

	return open(card->card, O_RDWR);
}

/**
 * igt_open_render:
 * @card: pointer to igt_device_card structure
 *
 * Open /dev/dri/renderDX device represented by igt_device_card structure.
 * Requires filled @card argument (see igt_device_card_match() function).
 *
 * An open DRM fd or -1 on error
 */
int igt_open_render(struct igt_device_card *card)
{
	if (!card || !strlen(card->render))
		return -1;

	return open(card->render, O_RDWR);
}

/**
 * igt_device_prepare_filtered_view:
 * @vendor: name for GPUs vendor to search, eg. "intel"
 *
 * Filter GPU devices for given vendor or with supplied --device
 * option or IGT_DEVICE environment variable.
 *
 * Returns:
 * Number of filtered GPUs for a vendor or number of filters.
 */
int igt_device_prepare_filtered_view(const char *vendor)
{
	int gpu_count;

	gpu_count = igt_device_filter_count();
	if (!gpu_count) {
		char gpu_filter[256];

		igt_assert(vendor);
		if (!strcmp(vendor, "vgem") || !strcmp(vendor, "other")) {
			igt_debug("Unsupported vendor: %s\n", vendor);
			return 0;
		}

		if (!strcmp(vendor, "any")) {
			igt_debug("Chipset DRIVER_ANY unsupported without --device filters\n");
			return 0;
		}

		igt_assert(snprintf(gpu_filter, sizeof(gpu_filter), "pci:vendor=%s,card=all",
				    vendor) < sizeof(gpu_filter));

		igt_device_filter_add(gpu_filter); // fill-in filters for all GPUs
		gpu_count = igt_device_filter_count();
		igt_debug("Found %d GPUs for vendor: %s\n", gpu_count, vendor);
	} else {
		struct igt_device_card card;
		bool found;
		int count = 0;

		for (int i = 0; i < gpu_count; i++) {
			const char *filter;

			filter = igt_device_filter_get(i);
			found = igt_device_card_match(filter, &card);
			if (found && strlen(card.card)) {
				igt_debug("Found GPU%d card %s\n", i, card.card);
				++count;
			}
		}

		if (count < gpu_count) {
			igt_debug("Counted GPUs %d lower than number of filters %d\n", count, gpu_count);
			gpu_count = count;
		} else {
			igt_debug("Found %d GPUs\n", gpu_count);
		}
	}

	return gpu_count;
}
