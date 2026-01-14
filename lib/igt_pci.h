// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef __IGT_PCI_H__
#define __IGT_PCI_H__

#include <endian.h>
#include <stddef.h>
#include <stdint.h>

/* forward declaration */
struct pci_device;

#define PCI_TYPE0_1_HEADER_SIZE 0x40
#define PCI_CAPS_START 0x34
#define PCI_CFG_SPACE_SIZE 0x100

enum pci_cap_id {
	PCI_EXPRESS_CAP_ID = 0x10
};

#define PCI_DEVICE_TYPE_OFFSET 0x2
#define PCI_DEVICE_TYPE_UPSTREAM_PORT	0x5
#define PCI_SLOT_CAP_OFFSET 0x14
#define  PCI_SLOT_PWR_CTRL_PRESENT (1 << 1)

int find_pci_cap_offset(struct pci_device *dev, enum pci_cap_id cap_id);
int igt_pci_device_unbind(const char *pci_slot);
int igt_pci_driver_bind(const char *driver, const char *pci_slot);
int igt_pci_driver_unbind(const char *driver, const char *pci_slot);
int igt_pci_driver_unbind_all(const char *driver);
int igt_pci_set_driver_override(const char *pci_slot, const char *driver);
int igt_pci_probe_drivers(const char *pci_slot);
int igt_pci_get_bound_driver_name(const char *pci_slot, char *driver, size_t driver_len);
int igt_pci_bind_driver_override(const char *pci_slot, const char *driver,
				 unsigned int timeout_ms);
int igt_pci_unbind_driver_override(const char *pci_slot, unsigned int timeout_ms);

#endif
