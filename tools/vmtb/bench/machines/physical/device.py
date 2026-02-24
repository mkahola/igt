# SPDX-License-Identifier: MIT
# Copyright © 2024-2026 Intel Corporation

import importlib
import logging
import re
from pathlib import Path
from typing import Any, List

from bench import exceptions
from bench.configurators import pci
from bench.configurators.vgpu_profile import (VgpuProfile, VgpuResourcesConfig,
                                              VgpuSchedulerConfig)
from bench.drivers.driver_interface import PfDriverInterface, SchedulingPriority
from bench.helpers.log import LogDecorators
from bench.machines.device_interface import DeviceInterface

logger = logging.getLogger('Device')


class Device(DeviceInterface):
    class PciInfo:
        def __init__(self, bdf: str) -> None:
            self.bdf: str = bdf
            self.devid: str = self.get_device_id(self.bdf)
            self.minor_number: int = self.get_device_minor_number(self.bdf)

        def get_device_minor_number(self, bdf: str) -> int:
            drm_dir = Path('/sys/bus/pci/devices/') / bdf / 'drm'

            for file_path in drm_dir.iterdir():
                if file_path.match('card*'):
                    index_match = re.search(r'card(?P<card_index>\d+)', file_path.name)
                    if index_match:
                        return int(index_match.group('card_index'))

            logger.error("Could not determine card index for device %s", bdf)
            raise exceptions.HostError(f'Could not determine card index for device {bdf}')

        def get_device_id(self, bdf: str) -> str:
            device_file = Path('/sys/bus/pci/devices/') / bdf / 'device'
            devid = device_file.read_text()

            return devid.strip()[2:] # Strip whitespaces and 0x

    def __init__(self, bdf: str, driver: str) -> None:
        self.pci_info = self.PciInfo(bdf)
        self.gpu_model = pci.get_gpu_model(self.pci_info.devid)
        self.driver: PfDriverInterface = self.instantiate_driver(self.pci_info.bdf, driver)

    def __str__(self) -> str:
        return f'Dev-{self.pci_info.bdf}'

    def instantiate_driver(self, bdf: str, driver_name: str) -> Any:
        module_name = f'bench.drivers.{driver_name}'
        class_name = f'{driver_name.capitalize()}Driver'

        try:
            driver_module = importlib.import_module(module_name)
            driver_class = getattr(driver_module, class_name)
        except (ImportError, AttributeError) as exc:
            logging.error("Driver module/class is not available: %s", exc)
            raise exceptions.VmtbConfigError(f'Requested driver module {driver_name} is not available!')

        return driver_class(bdf)

    def set_drivers_autoprobe(self, val: bool) -> None:
        self.driver.set_drivers_autoprobe(int(val))
        ret = self.driver.get_drivers_autoprobe()
        if ret != int(val):
            logger.error("Autoprobe value mismatch - requested: %s, got: %s", val, ret)
            raise exceptions.HostError(f'Autoprobe value mismatch - requested: {val}, got: {ret}')

    def get_total_vfs(self) -> int:
        return self.driver.get_totalvfs()

    def get_current_vfs(self) -> int:
        return self.driver.get_numvfs()

    def get_num_gts(self) -> int:
        return self.driver.get_num_gts()

    def has_lmem(self) -> bool:
        return self.driver.has_lmem()

    def is_gt_media_type(self, gt_num: int) -> bool:
        return self.driver.is_media_gt(gt_num)

    def create_vf(self, num: int) -> int:
        """Enable a requested number of VFs.
        Disable SRIOV drivers autoprobe to allow VFIO driver override for VFs.
        """
        logger.info("[%s] Enable %s VFs", self.pci_info.bdf, num)
        if self.get_current_vfs() != 0:
            self.remove_vfs()

        self.numvf = num

        # Disable driver autoprobe to avoid driver load on VF (override to vfio is required)
        logger.debug("[%s] Disable drivers autoprobe", self.pci_info.bdf)
        self.set_drivers_autoprobe(False)

        self.driver.set_numvfs(num)
        ret = self.driver.get_numvfs()
        assert ret == num

        return ret

    def remove_vfs(self) -> int:
        """Disable all existing VFs.
        Re-enable SRIOV drivers autoprobe.
        """
        logger.info("[%s] Disable VFs", self.pci_info.bdf)
        self.driver.set_numvfs(0)
        ret = self.driver.get_numvfs()
        if ret != 0:
            raise exceptions.HostError('VFs not disabled after 0 write')

        logger.debug("[%s] Enable drivers autoprobe", self.pci_info.bdf)
        self.set_drivers_autoprobe(True)

        return ret

    def bind_driver(self) -> None:
        self.driver.bind(self.pci_info.bdf)

    def unbind_driver(self) -> None:
        self.driver.unbind(self.pci_info.bdf)

    def override_vf_driver(self, vf_num: int) -> str:
        """Set VFIO as VF driver."""
        pci_devices_path = Path('/sys/bus/pci/devices/')
        vfio_driver = f'{self.driver.get_name()}-vfio-pci'
        if not Path(f'/sys/bus/pci/drivers/{vfio_driver}').exists():
            vfio_driver = 'vfio-pci'

        # virtfnN is a symlink - get the last part of the absolute path, ie. VF BDF like 00:12:00.1
        # TODO: replace by Path.readlink() when Python 3.9 supported
        pass_vf_bdf = (pci_devices_path / self.pci_info.bdf / f'virtfn{vf_num - 1}').resolve().name
        override_path = pci_devices_path / pass_vf_bdf / 'driver_override'
        override_path.write_text(vfio_driver, encoding='utf-8')
        logger.debug("VF%s VFIO driver: %s", vf_num, override_path.read_text())

        return pass_vf_bdf

    @LogDecorators.parse_kmsg
    def get_vf_bdf(self, vf_num: int) -> str:
        """Provide BDF of VF prepared for pass to VM - with VFIO driver override and probe."""
        pass_vf_bdf = self.override_vf_driver(vf_num)

        drivers_probe = Path('/sys/bus/pci/drivers_probe')
        drivers_probe.write_text(pass_vf_bdf, encoding='utf-8')

        logger.info("[%s] VF%s ready for pass to VM", pass_vf_bdf, vf_num)
        return pass_vf_bdf

    def get_vfs_bdf(self, *args: int) -> List[str]:
        vf_list = list(set(args))
        bdf_list = [self.get_vf_bdf(vf) for vf in vf_list]
        return bdf_list

    def __set_provisioning(self, num_vfs: int, profile: VgpuProfile, set_resources: bool) -> None:
        """Helper to write provisioning attributes over sysfs/debugfs for PF and requested number of VFs.
        If 'set_resources' parameter is True - apply the full vGPU profile (hard resources and scheduling).
        Otherwise, set only scheduling profile (e.g. in case of auto resources provisioning).
        """
        all_gt_nums = list(range(self.get_num_gts()))
        main_gt_nums = [gt_num for gt_num in all_gt_nums if not self.is_gt_media_type(gt_num)]
        logger.info("[%s] Provision %sxVFs on main GT%s", self.pci_info.bdf, num_vfs, main_gt_nums)

        for gt_num in all_gt_nums:
            if set_resources:
                self.set_resources(0, gt_num, profile.resources)
            self.driver.set_pf_policy_reset_engine(gt_num, int(profile.security.reset_after_vf_switch))

        self.set_scheduling(0, profile.scheduler)

        for vf_num in range(1, num_vfs + 1):
            if len(main_gt_nums) > 1 and num_vfs > 1:
                # Multi-tile device Mode 2|3 - odd VFs on GT0, even on GT1
                all_gt_nums = [main_gt_nums[0] if vf_num % 2 else main_gt_nums[1]]

            for gt_num in all_gt_nums:
                if set_resources:
                    self.set_resources(vf_num, gt_num, profile.resources)

            self.set_scheduling(vf_num, profile.scheduler)

    def provision(self, profile: VgpuProfile) -> None:
        """Provision PF and VF(s) based on requested vGPU profile."""
        self.__set_provisioning(profile.num_vfs, profile, set_resources=True)

    def provision_scheduling(self, num_vfs: int, profile: VgpuProfile) -> None:
        """Provision PF and VF(s) scheduling based on requested vGPU profile's scheduler config."""
        self.__set_provisioning(num_vfs, profile, set_resources=False)

    # fn_num = 0 for PF, 1..n for VF
    def set_scheduling(self, fn_num: int, scheduling_config: VgpuSchedulerConfig) -> None:
        """Write sysfs PF/VF scheduling attributes."""
        logger.debug("[%s] Set scheduling for PCI Function %s", self.pci_info.bdf, fn_num)

        if fn_num == 0:
            eq, pt = scheduling_config.pfExecutionQuanta, scheduling_config.pfPreemptionTimeout
        else:
            eq, pt = scheduling_config.vfExecutionQuanta, scheduling_config.vfPreemptionTimeout

        self.driver.set_exec_quantum_ms(fn_num, eq)
        self.driver.set_preempt_timeout_us(fn_num, pt)

        if scheduling_config.scheduleIfIdle:
            self.driver.set_bulk_sched_priority(SchedulingPriority.NORMAL)

    def set_resources(self, fn_num: int, gt_num: int, resources_config: VgpuResourcesConfig) -> None:
        """Write debugfs PF/VF resources attributes."""
        logger.debug("[%s] Set resources for PCI Function %s", self.pci_info.bdf, fn_num)
        if fn_num == 0:
            if not self.is_gt_media_type(gt_num):
                self.driver.set_pf_ggtt_spare(gt_num, resources_config.pfGgtt)
                self.driver.set_pf_lmem_spare(gt_num, resources_config.pfLmem)
            self.driver.set_pf_contexts_spare(gt_num, resources_config.pfContexts)
            self.driver.set_pf_doorbells_spare(gt_num, resources_config.pfDoorbells)
        else:
            if not self.is_gt_media_type(gt_num):
                self.driver.set_ggtt_quota(fn_num, gt_num, resources_config.vfGgtt)
                self.driver.set_lmem_quota(fn_num, gt_num, resources_config.vfLmem)
            self.driver.set_contexts_quota(fn_num, gt_num, resources_config.vfContexts)
            self.driver.set_doorbells_quota(fn_num, gt_num, resources_config.vfDoorbells)

    def clear_provisioning_attributes(self, num_vfs: int) -> None:
        """Clear provisioning attributes for a requested number of VFs."""
        # Provisioning config are likely wiped out by (xe) debugfs/restore_auto_provisioning,
        # but explicit clear shouldn't harm.
        self.driver.set_bulk_sched_priority(SchedulingPriority.LOW)
        self.driver.set_bulk_exec_quantum_ms(0)
        self.driver.set_bulk_preempt_timeout_us(0)

        for gt_num in range(self.get_num_gts()):
            self.driver.set_pf_policy_reset_engine(gt_num, 0)
            self.driver.set_doorbells_quota(0, gt_num, 0)
            # PF contexts cannot be set from sysfs

            for vf_num in range(1, num_vfs + 1):
                self.driver.set_contexts_quota(vf_num, gt_num, 0)
                self.driver.set_doorbells_quota(vf_num, gt_num, 0)
                if not self.is_gt_media_type(gt_num):
                    self.driver.set_ggtt_quota(vf_num, gt_num, 0)
                    self.driver.set_lmem_quota(vf_num, gt_num, 0)

    def reset_provisioning(self, num_vfs: int) -> None:
        """Clear provisioning config for a given number of VFs and restore auto provisioning mode."""
        logger.info("[%s] Reset %sxVF provisioning configuration", self.pci_info.bdf, num_vfs)
        self.clear_provisioning_attributes(num_vfs)
        self.driver.restore_auto_provisioning()

    def cancel_work(self) -> None:
        """Drop and reset remaining GPU execution at exit."""
        self.driver.cancel_work()

    def get_scheduling_priority(self, fn_num: int) -> SchedulingPriority:
        """Get scheduling priority for a given VF or PF."""
        return self.driver.get_sched_priority(fn_num)

    def set_scheduling_priority(self, val: SchedulingPriority) -> None:
        """Set scheduling priority for PF and all VFs. Normal priority enables strict scheduling."""
        self.driver.set_bulk_sched_priority(val)

    def set_pf_scheduling_priority(self, val: SchedulingPriority) -> None:
        """Set scheduling priority for PF only. High prioritizes PF execution over VFs."""
        self.driver.set_pf_sched_priority(val)
