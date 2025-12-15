/*
 * Copyright © 2016 Intel Corporation
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
 */
#include "igt.h"
#include <dirent.h>
#include <sys/utsname.h>
/**
 * TEST: i915 module load
 * Description: Tests the i915 module loading.
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: driver
 * Functionality: driver handler
 * Feature: core
 * Test category: GEM_Legacy
 *
 * SUBTEST: fault-injection
 * Description: Verify that i915 driver can be successfully bound and unbound with fault injection.
 * Functionality: fault
 *
 * SUBTEST: load
 * Description: Check if i915 and friends are not yet loaded, then load them.
 *
 * SUBTEST: reload
 * Description: Verify the basic functionality of i915 driver after it's reloaded.
 * Feature: core, sriov-core
 *
 * SUBTEST: reload-no-display
 * Description: Verify that i915 driver can be successfully loaded with disabled display.
 * Feature: core, sriov-core
 *
 * SUBTEST: resize-bar
 * Description: Check whether lmem bar size can be resized to only supported sizes.
 */

#ifdef __linux__
#include <linux/limits.h>
#endif
#include <signal.h>
#include <libgen.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#include "i915/gem.h"
#include "i915/gem_create.h"
#include "igt_debugfs.h"
#include "igt_aux.h"
#include "igt_kmod.h"
#include "igt_sysfs.h"
#include "igt_core.h"
#include "igt_device.h"

#define BAR_SIZE_SHIFT 20
#define MIN_BAR_SIZE 256

IGT_TEST_DESCRIPTION("Tests the i915 module loading.");

static void store_all(int i915)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(i915));
	uint32_t engines[I915_EXEC_RING_MASK + 1];
	uint32_t batch[16];
	uint64_t ahnd, offset, bb_offset;
	unsigned int sz = ALIGN(sizeof(batch) * ARRAY_SIZE(engines), 4096);
	struct drm_i915_gem_relocation_entry reloc = {
		.offset = sizeof(uint32_t),
		.read_domains = I915_GEM_DOMAIN_RENDER,
		.write_domain = I915_GEM_DOMAIN_RENDER,
	};
	struct drm_i915_gem_exec_object2 obj[2] = {
		{
			.handle = gem_create(i915, sizeof(engines)),
			.flags = EXEC_OBJECT_WRITE,
		},
		{
			.handle = gem_create(i915, sz),
			.relocation_count = 1,
			.relocs_ptr = to_user_pointer(&reloc),
		},
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(obj),
		.buffer_count = 2,
	};
	const struct intel_execution_engine2 *e;
	const intel_ctx_t *ctx;
	int reloc_sz = sizeof(uint32_t);
	unsigned int nengine, value;
	void *cs;
	int i;

	i = 0;
	batch[i] = MI_STORE_DWORD_IMM_GEN4 | (gen < 6 ? 1 << 22 : 0);
	if (gen >= 8) {
		batch[++i] = 0;
		batch[++i] = 0;
		reloc_sz = sizeof(uint64_t);
	} else if (gen >= 4) {
		batch[++i] = 0;
		batch[++i] = 0;
		reloc.offset += sizeof(uint32_t);
	} else {
		batch[i]--;
		batch[++i] = 0;
	}
	batch[value = ++i] = 0xc0ffee;
	batch[++i] = MI_BATCH_BUFFER_END;

	nengine = 0;
	cs = gem_mmap__device_coherent(i915, obj[1].handle, 0, sz, PROT_WRITE);

	ctx = intel_ctx_create_all_physical(i915);
	ahnd = get_reloc_ahnd(i915, ctx->id);
	if (ahnd)
		obj[1].relocation_count = 0;
	bb_offset = get_offset(ahnd, obj[1].handle, sz, 4096);
	offset = get_offset(ahnd, obj[0].handle, sizeof(engines), 0);

	for_each_ctx_engine(i915, ctx, e) {
		uint64_t addr;

		igt_assert(reloc.presumed_offset != -1);
		addr = reloc.presumed_offset + reloc.delta;

		if (!gem_class_can_store_dword(i915, e->class))
			continue;

		if (ahnd) {
			i = 1;
			batch[i++] = offset + reloc.delta;
			batch[i++] = offset >> 32;
			obj[0].offset = offset;
			obj[0].flags |= EXEC_OBJECT_PINNED;
			obj[1].offset = bb_offset;
			obj[1].flags |= EXEC_OBJECT_PINNED;
		}

		batch[value] = nengine;

		execbuf.flags = e->flags;
		if (gem_store_dword_needs_secure(i915))
			execbuf.flags |= I915_EXEC_SECURE;
		execbuf.flags |= I915_EXEC_NO_RELOC | I915_EXEC_HANDLE_LUT;
		execbuf.rsvd1 = ctx->id;

		memcpy(cs + execbuf.batch_start_offset, batch, sizeof(batch));
		if (!ahnd)
			memcpy(cs + reloc.offset, &addr, reloc_sz);
		gem_execbuf(i915, &execbuf);

		if (++nengine == ARRAY_SIZE(engines))
			break;

		reloc.delta += sizeof(uint32_t);
		reloc.offset += sizeof(batch);
		execbuf.batch_start_offset += sizeof(batch);
	}
	munmap(cs, sz);
	gem_close(i915, obj[1].handle);

	memset(engines, 0xdeadbeef, sizeof(engines));
	gem_read(i915, obj[0].handle, 0, engines, nengine * sizeof(engines[0]));
	gem_close(i915, obj[0].handle);
	intel_ctx_destroy(i915, ctx);
	put_offset(ahnd, obj[0].handle);
	put_offset(ahnd, obj[1].handle);
	put_ahnd(ahnd);

	for (i = 0; i < nengine; i++)
		igt_assert_eq_u32(engines[i], i);
}

static void unload_or_die(const char *module_name)
{
	int err, loop;

	/* should be unloaded, so expect a no-op */
	for (loop = 0;; loop++) {
		err = igt_kmod_unload(module_name);
		if (err == -ENOENT) /* -ENOENT == unloaded already */
			err = 0;
		if (!err || loop >= 10)
			break;

		sleep(1); /* wait for external clients to drop */
		if (!strcmp(module_name, "i915"))
			igt_i915_driver_unload();
	}

	igt_abort_on_f(err,
		       "Failed to unload '%s' err:%d after %ds, leaving dangerous modparams intact!\n",
		       module_name, err, loop);
}

static void gem_sanitycheck(void)
{
	struct drm_i915_gem_busy args = {};
	int i915 = __drm_open_driver(DRIVER_INTEL);
	int expected = -ENOENT;
	int err;

	err = 0;
	if (ioctl(i915,DRM_IOCTL_I915_GEM_BUSY, &args))
		err = -errno;
	if (err == expected)
		store_all(i915);
	errno = 0;

	drm_close_driver(i915);
	igt_assert_eq(err, expected);
}

static void
hda_dynamic_debug(bool enable)
{
	FILE *fp;
	const char snd_hda_intel_on[] = "module snd_hda_intel +pf";
	const char snd_hda_core_on[] = "module snd_hda_core +pf";

	const char snd_hda_intel_off[] = "module snd_hda_core =_";
	const char snd_hda_core_off[] = "module snd_hda_intel =_";

	fp = fopen("/sys/kernel/debug/dynamic_debug/control", "w");
	if (!fp) {
		igt_debug("hda dynamic debug not available\n");
		return;
	}

	if (enable) {
		fwrite(snd_hda_intel_on, 1, sizeof(snd_hda_intel_on), fp);
		fwrite(snd_hda_core_on, 1, sizeof(snd_hda_core_on), fp);
	} else {
		fwrite(snd_hda_intel_off, 1, sizeof(snd_hda_intel_off), fp);
		fwrite(snd_hda_core_off, 1, sizeof(snd_hda_core_off), fp);
	}

	fclose(fp);
}

static void load_and_check_i915(void)
{
	int error;
	int drm_fd;

	hda_dynamic_debug(true);
	error = igt_i915_driver_load(NULL);
	hda_dynamic_debug(false);

	igt_assert_eq(error, 0);

	/* driver is ready, check if it's bound */
	drm_fd = __drm_open_driver(DRIVER_INTEL);
	igt_fail_on_f(drm_fd < 0, "Cannot open the i915 DRM driver after modprobing i915.\n");

	/* make sure the GPU is idle */
	gem_quiescent_gpu(drm_fd);
	drm_close_driver(drm_fd);

	/* make sure we can do basic memory ops */
	gem_sanitycheck();
}

static uint32_t  driver_load_with_lmem_bar_size(uint32_t lmem_bar_size, bool check_support)
{
	int i915 = -1;
	char lmem_bar[64];

	igt_i915_driver_unload();
	if (lmem_bar_size == 0)
		igt_assert_eq(igt_i915_driver_load(NULL), 0);
	else {
		sprintf(lmem_bar, "lmem_bar_size=%u", lmem_bar_size);
		igt_assert_eq(igt_i915_driver_load(lmem_bar), 0);
	}

	i915 = __drm_open_driver(DRIVER_INTEL);
	igt_require_fd(i915);
	igt_require_gem(i915);
	igt_require(gem_has_lmem(i915));

	if (check_support) {
		char *tmp;

		tmp = __igt_params_get(i915, "lmem_bar_size");
		igt_skip_on_f(!tmp,
			      "lmem_bar_size modparam not supported on this kernel. Skipping the test.\n");
		free(tmp);
	}

	for_each_memory_region(r, i915) {
		if (r->ci.memory_class == I915_MEMORY_CLASS_DEVICE) {
			lmem_bar_size = (r->cpu_size >> BAR_SIZE_SHIFT);

			igt_skip_on_f(lmem_bar_size == 0, "CPU visible size should be greater than zero. Skipping for older kernel.\n");
		}
	}

	drm_close_driver(i915);

	return lmem_bar_size;
}

struct fault_injection_params {
	/* @probability: Likelihood of failure injection, in percent. */
	uint32_t probability;
	/* @interval: Specifies the interval between failures */
	uint32_t interval;
	/* @times: Specifies how many times failures may happen at most */
	int32_t times;
	/*
	 * @space: Specifies how many times fault injection is suppressed before
	 * first injection
	 */
	uint32_t space;
};

static int fail_function_open(void)
{
	int debugfs_fail_function_dir_fd;
	const char *debugfs_root;
	char path[96];

	debugfs_root = igt_debugfs_mount();
	igt_assert(debugfs_root);

	sprintf(path, "%s/fail_function", debugfs_root);

	if (access(path, F_OK))
		return -1;

	debugfs_fail_function_dir_fd = open(path, O_RDONLY);
	igt_debug_on_f(debugfs_fail_function_dir_fd < 0, "path: %s\n", path);

	return debugfs_fail_function_dir_fd;
}

static void injection_list_add(const char function_name[])
{
	int dir;

	dir = fail_function_open();

	igt_assert_lte(0, dir);
	igt_assert_lte(0, igt_sysfs_printf(dir, "inject", "%s", function_name));

	close(dir);
}

/*
 * Default fault injection parameters which injects fault on first call to the
 * configured fail_function.
 */
static const struct fault_injection_params default_fault_params = {
	.probability = 100,
	.interval = 0,
	.times = -1,
	.space = 0
};

/*
 * See https://docs.kernel.org/fault-injection/fault-injection.html#application-examples
 */
static void setup_injection_fault(const struct fault_injection_params *fault_params)
{
	int dir;

	if (!fault_params)
		fault_params = &default_fault_params;

	igt_assert(fault_params->probability >= 0);
	igt_assert(fault_params->probability <= 100);

	dir = fail_function_open();
	igt_assert_lte(0, dir);

	igt_debug("probability = %d, interval = %d, times = %d, space = %u\n",
		  fault_params->probability, fault_params->interval,
		  fault_params->times, fault_params->space);

	igt_assert_lte(0, igt_sysfs_printf(dir, "task-filter", "N"));
	igt_sysfs_set_u32(dir, "probability", fault_params->probability);
	igt_sysfs_set_u32(dir, "interval", fault_params->interval);
	igt_sysfs_set_s32(dir, "times", fault_params->times);
	igt_sysfs_set_u32(dir, "space", fault_params->space);
	igt_sysfs_set_u32(dir, "verbose", 1);

	close(dir);
}

static void injection_list_clear(void)
{
	/* If nothing specified (‘’) injection list is cleared */
	return injection_list_add("");
}

/*
 * The injectable file requires CONFIG_FUNCTION_ERROR_INJECTION in kernel.
 */
static bool fail_function_injection_enabled(void)
{
	char *contents;
	int dir;

	dir = fail_function_open();
	if (dir < 0)
		return false;

	contents = igt_sysfs_get(dir, "injectable");
	if (!contents)
		return false;

	free(contents);
	close(dir);

	return true;
}

static void cleanup_injection_fault(int sig)
{
	injection_list_clear();
}

static void set_retval(const char function_name[], long long retval)
{
	char path[96];
	int dir;

	dir = fail_function_open();
	igt_assert_lte(0, dir);

	sprintf(path, "%s/retval", function_name);
	igt_assert_lte(0, igt_sysfs_printf(dir, path, "%#016llx", retval));

	close(dir);
}

static int
inject_fault_probe(int fd, const char pci_slot[], const char function_name[],
		   int inject_error, int devicefd)
{
	int err = 0;

	injection_list_add(function_name);
	set_retval(function_name, inject_error);

	igt_assert(igt_sysfs_set(devicefd, "reset", "1"));
	igt_kmod_bind("i915", pci_slot);

	err = -errno;
	injection_list_clear();
	return err;
}

static char *bus_addr(int fd, char *path)
{
	char sysfs[PATH_MAX];
	char *rp, *addr_pos;

	if (!igt_sysfs_path(fd, sysfs, sizeof(sysfs)))
		return NULL;

	if (PATH_MAX <= (strlen(sysfs) + strlen("/device")))
		return NULL;

	strcat(sysfs, "/device");

	rp = realpath(sysfs, path);

	igt_require(strstr(rp, "/sys/devices/pci") == rp);
	addr_pos = strrchr(rp, '/');

	snprintf(path, PATH_MAX, "%s", addr_pos + 1);
	return path;
}

static void test_fault_injection(void)
{
	char pci_slot[NAME_MAX], addr[PATH_MAX];
	int ret = 0;
	int sysfs = 0;
	int devicefd = 0;
	int fd, guc;

	char error_ignore[] =
		".*Failed to register driver for userspace access!"
		"|.*Device initialization failed.*"
		"|.*probe with driver i915 failed with error.*"
		"|.*GT1: GSC proxy handler failed to init.*"
		"|.*GT0: GUC: failed with -ENXIO.*"
		"|.*GuC fw rsa data creation failed -ENXIO.*"
		"|.*GUC: failed with -ENXIO.*"
		"|.*Failed to register driver for userspace access.*"
		"|.*GT0: GuC fw rsa data creation failed -ENXIO.*"
		;

	struct {
		const char *test_name;
		const char *function;
		int faultcode;
		int flags; // 0x1 = guc related tests
	} fail_tests[] = {
		{"__uc_init", "__uc_init", -ENOMEM, 1},
		{"i915_driver_mmio_probe", "i915_driver_mmio_probe", -ENODEV, 0},
		{"i915_driver_hw_probe", "i915_driver_hw_probe", -ENODEV, 0},
		{"i915_pci_probe", "i915_pci_probe", -ENODEV, 0},
		{"__fw_domain_init", "__fw_domain_init", -ENOMEM, 1},
		{"intel_connector_register", "intel_connector_register", -EFAULT, 0},
		{"i915_driver_early_probe", "i915_driver_early_probe", -ENODEV, 0},
		{"intel_gt_init-ENODEV", "intel_gt_init", -ENODEV, 1},
		{"intel_guc_ct_init", "intel_guc_ct_init", -ENXIO, 1},
		{"uc_fw_rsa_data_create", "uc_fw_rsa_data_create", -ENXIO, 1},
	};

	igt_require(fail_function_injection_enabled());

	fd = drm_open_driver(DRIVER_INTEL);
	igt_require(fd);

	guc = gem_submission_method(fd);

	sysfs = igt_sysfs_open(fd);
	devicefd = openat(sysfs, "device", O_DIRECTORY);
	bus_addr(fd, addr);
	igt_install_exit_handler(cleanup_injection_fault);

	igt_device_get_pci_slot_name(fd, pci_slot);

	for (int c = 0; c < ARRAY_SIZE(fail_tests); c++) {
		igt_dynamic(fail_tests[c].test_name) {
			if (fail_tests[c].flags & 0x1)
				igt_require(guc == GEM_SUBMISSION_GUC);

			igt_emit_ignore_dmesg_regex(error_ignore);

			igt_set_timeout(60, "Driver unbind re-bind timeout!");
			igt_kmod_unbind("i915", pci_slot);
			setup_injection_fault(&default_fault_params);

			ret = inject_fault_probe(fd, pci_slot,
						 fail_tests[c].function,
						 fail_tests[c].faultcode,
						 devicefd);

			igt_reset_timeout();

			if (ret == fail_tests[c].faultcode) {
				igt_info("Load failed with expected error %s (%d)\n",
					 strerror(-ret), ret);
			} else if (ret == 0) {
				igt_info("Load succeeded - %s() was *not* executed!\n",
					 fail_tests[c].function);

				injection_list_clear();
				igt_assert_f(0, "%s() was *not* executed!\n",
					     fail_tests[c].function);

			} else {
				igt_info("Load failed with unexpected error %d (%s)\n",
					 ret, strerror(-ret));

				close(devicefd);
				close(sysfs);
				drm_close_driver(fd);

				igt_i915_driver_unload();
				igt_i915_driver_load(NULL);

				igt_assert_f(0, "unexpected error %d (%s)\n",
					     ret, strerror(-ret));
			}
		}
	}

	close(devicefd);
	close(sysfs);
	drm_close_driver(fd);

	unload_or_die("i915");
	igt_i915_driver_load(NULL);
}

int igt_main()
{
	igt_describe("Check if i915 and friends are not yet loaded, then load them.");
	igt_subtest("load") {
		const char * unwanted_drivers[] = {
			"i915",
			"intel-gtt",
			NULL
		};

		for (int i = 0; unwanted_drivers[i] != NULL; i++) {
			igt_skip_on_f(igt_kmod_is_loaded(unwanted_drivers[i]),
			              "%s is already loaded\n", unwanted_drivers[i]);
		}

		load_and_check_i915();
	}

	igt_describe("Verify the basic functionality of i915 driver after it's reloaded.");
	igt_subtest("reload") {
		igt_i915_driver_unload();

		load_and_check_i915();

		/* only default modparams, can leave module loaded */
	}

	igt_describe("Verify that i915 driver can be successfully loaded with disabled display.");
	igt_subtest("reload-no-display") {
		igt_i915_driver_unload();

		igt_assert_eq(igt_i915_driver_load("disable_display=1"), 0);

		igt_i915_driver_unload();
	}

	igt_describe("Test i915 fault injection");
	igt_subtest_with_dynamic_f("fault-injection")
		test_fault_injection();

	igt_describe("Check whether lmem bar size can be resized to only supported sizes.");
	igt_subtest("resize-bar") {
		uint32_t result_bar_size;
		uint32_t lmem_bar_size;
		int i915 = -1;

		if (igt_kmod_is_loaded("i915")) {
			i915 = __drm_open_driver(DRIVER_INTEL);
			igt_require_fd(i915);
			igt_require_gem(i915);
			igt_require(gem_has_lmem(i915));
			igt_skip_on_f(igt_sysfs_get_num_gt(i915) > 1, "Skips for more than one lmem instance.\n");
			drm_close_driver(i915);
		}

		/* Test for lmem_bar_size modparam support */
		lmem_bar_size = driver_load_with_lmem_bar_size(MIN_BAR_SIZE, true);
		igt_skip_on_f(lmem_bar_size != MIN_BAR_SIZE, "Device lacks PCI resizeable BAR support.\n");

		lmem_bar_size = driver_load_with_lmem_bar_size(0, false);

		lmem_bar_size = roundup_power_of_two(lmem_bar_size);

		igt_skip_on_f(lmem_bar_size == MIN_BAR_SIZE, "Bar is already set to minimum size.\n");

		while (lmem_bar_size > MIN_BAR_SIZE) {
			lmem_bar_size = lmem_bar_size >> 1;

			result_bar_size = driver_load_with_lmem_bar_size(lmem_bar_size, false);

			igt_assert_f(lmem_bar_size == result_bar_size, "Bar couldn't be resized.\n");
		}

		/* Test with unsupported sizes */
		lmem_bar_size = 80;
		result_bar_size = driver_load_with_lmem_bar_size(lmem_bar_size, false);
		igt_assert_f(lmem_bar_size != result_bar_size, "Bar resized to unsupported size.\n");

		lmem_bar_size = 16400;
		result_bar_size = driver_load_with_lmem_bar_size(lmem_bar_size, false);
		igt_assert_f(lmem_bar_size != result_bar_size, "Bar resized to unsupported size.\n");

		igt_i915_driver_unload();
	}

	/* Subtests should unload the module themselves if they use modparams */
}
