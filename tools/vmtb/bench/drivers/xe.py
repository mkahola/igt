# SPDX-License-Identifier: MIT
# Copyright © 2024-2026 Intel Corporation

import logging
import re
import typing
from pathlib import Path

from bench.drivers.driver_interface import (PfDriverInterface,
                                            SchedulingPriority, VfControl)

logger = logging.getLogger('XeDriver')


class XeDriver(PfDriverInterface):
    """Xe driver abstraction class, implements PfDriverInterface.
    Provide xe specific sysfs/debugfs access and other operations on Host.
    """
    def get_name(self) -> str:
        return 'xe'

    def get_totalvfs(self) -> int:
        return int(self.read_sysfs(self.sysfs_device_path / 'sriov_totalvfs'))

    def get_numvfs(self) -> int:
        return int(self.read_sysfs(self.sysfs_device_path / 'sriov_numvfs'))

    def set_numvfs(self, val: int) -> None:
        self.write_sysfs(self.sysfs_device_path / 'sriov_numvfs', str(val))

    def get_drivers_autoprobe(self) -> int:
        return int(self.read_sysfs(self.sysfs_device_path / 'sriov_drivers_autoprobe'))

    def set_drivers_autoprobe(self, val: int) -> None:
        self.write_sysfs(self.sysfs_device_path / 'sriov_drivers_autoprobe', str(val))

    def get_num_gts(self) -> int:
        gt_num = 0
        # Fixme: tile0 only at the moment, add support for multiple tiles if needed
        path = self.sysfs_device_path / 'tile0' / 'gt'

        if path.exists():
            gt_num = 1
        else:
            while Path(f'{path}{gt_num}').exists():
                gt_num += 1

        return gt_num

    def has_lmem(self) -> bool:
        # XXX: is this a best way to check if LMEM is present?
        path = self.debugfs_path / 'gt0' / 'pf' / 'lmem_spare'
        return path.exists()

    def is_media_gt(self, gt_num: int) -> bool:
        # XXX: is lack of PF's ggtt/lmem_spare or VF's ggtt/lmem_quota
        # a best way to check for standalone media GT?
        path = self.debugfs_path / f'gt{gt_num}' / 'pf' / 'ggtt_spare'
        return not path.exists()

    def restore_auto_provisioning(self) -> None:
        path = self.debugfs_path / 'sriov' / 'restore_auto_provisioning'
        self.write_debugfs(str(path), str(1))

    def cancel_work(self) -> None:
        # Function to cancel all remaing work on GPU (for test cleanup).
        # Forcing reset (debugfs/gtM/force_reset_sync) shouldn't be used to idle GPU.
        pass

    # Create debugfs path to given parameter (without a base part):
    # gt@gt_num/[pf|vf@fn_num]/@attr
    # @fn_num: VF number (1-based) or 0 for PF
    # @gt_num: GT instance number
    # @subdir: subdirectory for attribute or empty string if not exists
    # @attr: iov parameter name
    # Returns: iov debugfs path to @attr
    def __helper_create_debugfs_path(self, fn_num: int, gt_num: int, subdir: str, attr: str) -> str:
        gt_fn_part = f'gt{gt_num}/pf' if fn_num == 0 else f'gt{gt_num}/vf{fn_num}'
        return f'{gt_fn_part}/{subdir}/{attr}'

    # Create sysfs sriov_admin path to given scheduling parameter (without a base part):
    # sriov_admin/[pf|vf@fn_num]/profile/@attr
    # @fn_num: VF number (1-based) or 0 for PF
    # @attr: iov parameter name
    # Returns: iov sysfs path to @attr
    def __helper_create_sriov_admin_path(self, fn_num: int, attr: str) -> str:
        fn_part = 'pf' if fn_num == 0 else f'vf{fn_num}'
        return f'sriov_admin/{fn_part}/profile/{attr}'

    # PF spare resources
    # Debugfs location: [SRIOV debugfs base path]/gtM/pf/xxx_spare
    def get_pf_ggtt_spare(self, gt_num: int) -> int:
        path = self.__helper_create_debugfs_path(0, gt_num, '', 'ggtt_spare')
        return int(self.read_debugfs(path))

    def set_pf_ggtt_spare(self, gt_num: int, val: int) -> None:
        path = self.__helper_create_debugfs_path(0, gt_num, '',  'ggtt_spare')
        self.write_debugfs(path, str(val))

    def get_pf_lmem_spare(self, gt_num: int) -> int:
        path = self.__helper_create_debugfs_path(0, gt_num, '', 'lmem_spare')
        return int(self.read_debugfs(path)) if self.has_lmem() else 0

    def set_pf_lmem_spare(self, gt_num: int, val: int) -> None:
        path = self.__helper_create_debugfs_path(0, gt_num, '', 'lmem_spare')
        if self.has_lmem():
            self.write_debugfs(path, str(val))

    def get_pf_contexts_spare(self, gt_num: int) -> int:
        path = self.__helper_create_debugfs_path(0, gt_num, '', 'contexts_spare')
        return int(self.read_debugfs(path))

    def set_pf_contexts_spare(self, gt_num: int, val: int) -> None:
        path = self.__helper_create_debugfs_path(0, gt_num, '', 'contexts_spare')
        self.write_debugfs(path, str(val))

    def get_pf_doorbells_spare(self, gt_num: int) -> int:
        path = self.__helper_create_debugfs_path(0, gt_num, '', 'doorbells_spare')
        return int(self.read_debugfs(path))

    def set_pf_doorbells_spare(self, gt_num: int, val: int) -> None:
        path = self.__helper_create_debugfs_path(0, gt_num, '', 'doorbells_spare')
        self.write_debugfs(path, str(val))

    # PF specific provisioning parameters
    # Debugfs location: [SRIOV debugfs base path]/gtM/pf
    def get_pf_policy_reset_engine(self, gt_num: int) -> int:
        path = self.__helper_create_debugfs_path(0, gt_num, '', 'reset_engine')
        return int(self.read_debugfs(path))

    def set_pf_policy_reset_engine(self, gt_num: int, val: int) -> None:
        path = self.__helper_create_debugfs_path(0, gt_num, '', 'reset_engine')
        self.write_debugfs(path, str(val))

    def get_pf_policy_sample_period_ms(self, gt_num: int) -> int:
        path = self.__helper_create_debugfs_path(0, gt_num, '', 'sample_period_ms')
        return int(self.read_debugfs(path))

    def set_pf_policy_sample_period_ms(self, gt_num: int, val: int) -> None:
        path = self.__helper_create_debugfs_path(0, gt_num, '', 'sample_period_ms')
        self.write_debugfs(path, str(val))

    # VF and PF provisioning parameters
    # Debugfs location: [SRIOV debugfs base path]/gtM/[pf|vfN]
    # @vf_num: VF number (1-based) or 0 for PF
    def get_ggtt_quota(self, vf_num: int, gt_num: int) -> int:
        if vf_num == 0:
            logger.warning("PF ggtt_quota not available")
            return 0

        path = self.__helper_create_debugfs_path(vf_num, gt_num, '', 'ggtt_quota')
        return int(self.read_debugfs(path))

    def set_ggtt_quota(self, vf_num: int, gt_num: int, val: int) -> None:
        if vf_num == 0:
            logger.warning("PF ggtt_quota not available")
            return

        path = self.__helper_create_debugfs_path(vf_num, gt_num, '', 'ggtt_quota')
        self.write_debugfs(path, str(val))

    def get_lmem_quota(self, vf_num: int, gt_num: int) -> int:
        if vf_num == 0:
            logger.warning("PF lmem_quota not available")
            return 0

        path = self.__helper_create_debugfs_path(vf_num, gt_num, '', 'lmem_quota')
        return int(self.read_debugfs(path)) if self.has_lmem() else 0

    def set_lmem_quota(self, vf_num: int, gt_num: int, val: int) -> None:
        if vf_num == 0:
            logger.warning("PF lmem_quota not available")
            return

        path = self.__helper_create_debugfs_path(vf_num, gt_num, '', 'lmem_quota')
        if self.has_lmem():
            self.write_debugfs(path, str(val))

    def get_contexts_quota(self, vf_num: int, gt_num: int) -> int:
        if vf_num == 0:
            logger.warning("PF contexts_quota not available")
            return 0

        path = self.__helper_create_debugfs_path(vf_num, gt_num, '', 'contexts_quota')
        return int(self.read_debugfs(path))

    def set_contexts_quota(self, vf_num: int, gt_num: int, val: int) -> None:
        if vf_num == 0:
            logger.warning("PF contexts_quota not available")
            return

        path = self.__helper_create_debugfs_path(vf_num, gt_num, '', 'contexts_quota')
        self.write_debugfs(path, str(val))

    def get_doorbells_quota(self, vf_num: int, gt_num: int) -> int:
        if vf_num == 0:
            logger.warning("PF doorbells_quota not available")
            return 0

        path = self.__helper_create_debugfs_path(vf_num, gt_num, '', 'doorbells_quota')
        return int(self.read_debugfs(path))

    def set_doorbells_quota(self, vf_num: int, gt_num: int, val: int) -> None:
        if vf_num == 0:
            logger.warning("PF doorbells_quota not available")
            return

        path = self.__helper_create_debugfs_path(vf_num, gt_num, '', 'doorbells_quota')
        self.write_debugfs(path, str(val))

    # VF and PF scheduling parameters
    # Sysfs location: [SRIOV sysfs base path]/sriov_admin/[pf|vfN]/profile
    # @fn_num: VF number (1-based) or 0 for PF
    def get_exec_quantum_ms(self, fn_num: int) -> int:
        sriov_admin_path = self.__helper_create_sriov_admin_path(fn_num, 'exec_quantum_ms')
        return int(self.read_sysfs(self.sysfs_device_path / sriov_admin_path))

    def set_exec_quantum_ms(self, fn_num: int, val: int) -> None:
        sriov_admin_path = self.__helper_create_sriov_admin_path(fn_num, 'exec_quantum_ms')
        self.write_sysfs(self.sysfs_device_path / sriov_admin_path, str(val))

    def set_bulk_exec_quantum_ms(self, val: int) -> None:
        self.write_sysfs(self.sysfs_device_path / 'sriov_admin/.bulk_profile/exec_quantum_ms', str(val))

    def get_preempt_timeout_us(self, fn_num: int) -> int:
        sriov_admin_path = self.__helper_create_sriov_admin_path(fn_num, 'preempt_timeout_us')
        return int(self.read_sysfs(self.sysfs_device_path / sriov_admin_path))

    def set_preempt_timeout_us(self, fn_num: int, val: int) -> None:
        sriov_admin_path = self.__helper_create_sriov_admin_path(fn_num, 'preempt_timeout_us')
        self.write_sysfs(self.sysfs_device_path / sriov_admin_path, str(val))

    def set_bulk_preempt_timeout_us(self, val: int) -> None:
        self.write_sysfs(self.sysfs_device_path / 'sriov_admin/.bulk_profile/preempt_timeout_us', str(val))

    def get_sched_priority(self, fn_num: int) -> SchedulingPriority:
        sriov_admin_path = self.__helper_create_sriov_admin_path(fn_num, 'sched_priority')
        ret = self.read_sysfs(self.sysfs_device_path / sriov_admin_path).rstrip()

        match = re.search(r'\[(low|normal|high)\]', ret)
        if match:
            return SchedulingPriority(match.group(1))

        logger.error("Unexpected sched_priority value (must be low/normal/high)")
        raise ValueError('Unexpected sched_priority value (must be low/normal/high)')

    def set_pf_sched_priority(self, val: SchedulingPriority) -> None:
        # Independent sched_prio setting is available for PF only
        sriov_admin_path = self.__helper_create_sriov_admin_path(0, 'sched_priority')
        self.write_sysfs(self.sysfs_device_path / sriov_admin_path, val)

    def set_bulk_sched_priority(self, val: SchedulingPriority) -> None:
        self.write_sysfs(self.sysfs_device_path / 'sriov_admin/.bulk_profile/sched_priority', val)

    # Control state of the running VF (WO)
    # Debugfs location: [SRIOV debugfs base path]/gtM/vfN/control
    # Allows PF admin to pause, resume or stop handling
    # submission requests from given VF and clear provisioning.
    # control: "pause|resume|stop|clear"
    # For debug purposes only.
    def set_vf_control(self, vf_num: int, val: VfControl) -> None:
        path = self.__helper_create_debugfs_path(vf_num, 0, '', 'control')
        self.write_debugfs(path, val)

    # Read [attribute]_available value from debugfs:
    # /sys/kernel/debug/dri/[card_index]/gt@gt_num/pf/@attr_available
    # @gt_num: GT instance number
    # @attr: iov parameter name
    # Returns: total and available size for @attr
    def __helper_get_debugfs_available(self, gt_num: int, attr: str) -> typing.Tuple[int, int]:
        path = self.debugfs_path / f'gt{gt_num}' / 'pf' / f'{attr}_available'
        total = available = 0

        out = path.read_text()
        for line in out.splitlines():
            param, value = line.split(':')
            value = value.lstrip().split('\t')[0]

            if param == 'total':
                total = int(value)
            elif param == 'avail':
                available = int(value)

        return (total, available)

    # Resources total availability
    # Debugfs location: [SRIOV debugfs base path]/gtM/pf/
    def get_ggtt_available(self, gt_num: int) -> typing.Tuple[int, int]:
        """Get total and available GGTT size."""
        return self.__helper_get_debugfs_available(gt_num, 'ggtt')
