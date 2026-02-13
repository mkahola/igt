/*
 * Copyright © 2015 Intel Corporation
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

#ifndef IGT_PM_H
#define IGT_PM_H

#include <stdbool.h>
#include <stdint.h>

#include "igt_kms.h"

#define IGT_INTEL_PACKAGE_CSTATE_PATH  "pmc_core/package_cstate_show"

/* DC State Flags */
#define IGT_INTEL_CHECK_DC5       (1 << 0)
#define IGT_INTEL_CHECK_DC6       (1 << 1)
#define IGT_INTEL_CHECK_DC3CO     (1 << 2)

void igt_pm_enable_audio_runtime_pm(void);
void igt_pm_enable_sata_link_power_management(void);
void igt_pm_restore_sata_link_power_management(void);

/**
 * igt_runtime_pm_status:
 * @IGT_RUNTIME_PM_STATUS_ACTIVE: device is active
 * @IGT_RUNTIME_PM_STATUS_SUSPENDED: device is suspended
 * @IGT_RUNTIME_PM_STATUS_SUSPENDING: device is in the process of suspending
 * @IGT_RUNTIME_PM_STATUS_RESUMING: device is in the process of resuming
 * @IGT_RUNTIME_PM_STATUS_UNKNOWN: unknown runtime PM status
 *
 * Symbolic values for runtime PM device status.
 */
enum igt_runtime_pm_status {
	IGT_RUNTIME_PM_STATUS_ACTIVE,
	IGT_RUNTIME_PM_STATUS_SUSPENDED,
	IGT_RUNTIME_PM_STATUS_SUSPENDING,
	IGT_RUNTIME_PM_STATUS_RESUMING,
	IGT_RUNTIME_PM_STATUS_UNKNOWN,
};

/* PCI ACPI firmware node real state */
enum igt_acpi_d_state {
	IGT_ACPI_D0,
	IGT_ACPI_D1,
	IGT_ACPI_D2,
	IGT_ACPI_D3Hot,
	IGT_ACPI_D3Cold,
	IGT_ACPI_UNKNOWN_STATE,
};

struct	igt_pm_pci_dev_pwrattr {
	struct pci_device *pci_dev;
	char control[64];
	bool autosuspend_supported;
	char autosuspend_delay[64];
};

struct igt_device_card;

bool igt_setup_runtime_pm(int device);
void igt_disable_runtime_pm(void);
void igt_restore_runtime_pm(void);
enum igt_runtime_pm_status igt_get_runtime_pm_status(void);
bool igt_wait_for_pm_status(enum igt_runtime_pm_status status);
bool igt_pm_dmc_loaded(int debugfs);
bool igt_pm_pc8_plus_residencies_enabled(int msr_fd);
bool i915_output_is_lpsp_capable(int drm_fd, igt_output_t *output);
int igt_pm_get_pcie_acpihp_slot(struct pci_device *pci_dev);
bool igt_pm_acpi_d3cold_supported(struct pci_device *pci_dev);
enum igt_acpi_d_state
igt_pm_get_acpi_real_d_state(struct pci_device *pci_dev);
int igt_pm_get_autosuspend_delay(struct pci_device *pci_dev);
void igt_pm_set_autosuspend_delay(struct pci_device *pci_dev, int delay_ms);
void igt_pm_enable_pci_card_runtime_pm(struct pci_device *root,
				       struct pci_device *i915);
void igt_pm_get_d3cold_allowed(const char *pci_slot_name, uint32_t *value);
void igt_pm_set_d3cold_allowed(const char *pci_slot_name, uint32_t value);
void igt_pm_setup_pci_card_runtime_pm(struct pci_device *pci_dev);
void igt_pm_restore_pci_card_runtime_pm(void);
void igt_pm_print_pci_card_runtime_status(void);
bool i915_is_slpc_enabled_gt(int drm_fd, int gt);
bool i915_is_slpc_enabled(int drm_fd);
uint64_t igt_pm_get_runtime_suspended_time(struct pci_device *pci_dev);
uint64_t igt_pm_get_runtime_active_time(struct pci_device *pci_dev);
int igt_pm_get_runtime_usage(struct pci_device *pci_dev);
void igt_pm_ignore_slpc_efficient_freq(int i915, int gtfd, bool val);
bool igt_has_pci_pm_capability(struct pci_device *pci_dev);
void igt_pm_dpms_toggle(igt_output_t *output);
uint32_t igt_get_dc_counter(const char *dc_data);
bool igt_support_dc6(int debugfs_fd);
char *igt_get_dc6_counter(const char *buf);
uint32_t igt_read_dc_counter(int debugfs_fd, int dc_flag);
bool igt_dc_state_wait_entry(int debugfs_fd, int dc_flag, int prev_dc_count);
const char *igt_dc_state_name(int dc_flag);
void igt_require_dc_counter(int debugfs_fd, int dc_flag);
unsigned int igt_read_pkgc_counter(int debugfs_root_fd);

#endif /* IGT_PM_H */
