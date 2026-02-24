# SPDX-License-Identifier: MIT
# Copyright © 2024-2026 Intel Corporation

import logging
import typing
from pathlib import Path

from bench import exceptions
from bench.drivers.driver_interface import (DriverInterface,
                                            SchedulingPriority, VfControl)
from bench.helpers.log import LogDecorators

logger = logging.getLogger('XeDriver')


class XeDriver(DriverInterface):
    def __init__(self, card_index: int) -> None:
        self.sysfs_card_path = Path(f'/sys/class/drm/card{card_index}')
        self.debugfs_path = Path(f'/sys/kernel/debug/dri/{card_index}')

    @staticmethod
    def get_name() -> str:
        return 'xe'

    @LogDecorators.parse_kmsg
    def __write_fs(self, base_path: Path, name: str, value: str) -> None:
        path = base_path / name
        try:
            path.write_text(value)
            logger.debug("Write: %s -> %s", value, path)
        except Exception as exc:
            logger.error("Unable to write %s -> %s", value, path)
            raise exceptions.HostError(f'Could not write to {path}. Error: {exc}') from exc

    @LogDecorators.parse_kmsg
    def __read_fs(self,  base_path: Path, name: str) -> str:
        path = base_path / name
        try:
            ret = path.read_text()
        except Exception as exc:
            logger.error("Unable to read %s", path)
            raise exceptions.HostError(f'Could not read from {path}. Error: {exc}') from exc

        logger.debug("Read: %s -> %s", path, ret.strip())
        return ret

    def __write_sysfs(self, name: str, value: str) -> None:
        self.__write_fs(self.sysfs_card_path / 'device', name, value)

    def __read_sysfs(self, name: str) -> str:
        return str(self.__read_fs(self.sysfs_card_path / 'device', name))

    def __write_debugfs(self, name: str, value: str) -> None:
        self.__write_fs(self.debugfs_path, name, value)

    def __read_debugfs(self, name: str) -> str:
        return str(self.__read_fs(self.debugfs_path, name))

    def bind(self, bdf: str) -> None:
        self.__write_sysfs('driver/bind', bdf)

    def unbind(self, bdf: str) -> None:
        self.__write_sysfs('driver/unbind', bdf)

    def get_totalvfs(self) -> int:
        return int(self.__read_sysfs('sriov_totalvfs'))

    def get_numvfs(self) -> int:
        return int(self.__read_sysfs('sriov_numvfs'))

    def set_numvfs(self, val: int) -> None:
        self.__write_sysfs('sriov_numvfs', str(val))

    def get_drivers_autoprobe(self) -> int:
        return int(self.__read_sysfs('sriov_drivers_autoprobe'))

    def set_drivers_autoprobe(self, val: int) -> None:
        self.__write_sysfs('sriov_drivers_autoprobe', str(val))

    def get_num_gts(self) -> int:
        gt_num = 0
        # Fixme: tile0 only at the moment, add support for multiple tiles if needed
        path = self.sysfs_card_path / 'device' / 'tile0' / 'gt'

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

    def get_auto_provisioning(self) -> bool:
        raise exceptions.NotAvailableError('auto_provisioning attribute not available')

    def set_auto_provisioning(self, val: bool) -> None:
        # No-op - xe driver doesn't publish this attribute
        pass

    def cancel_work(self) -> None:
        # Function to cancel all remaing work on GPU (for test cleanup).
        # Forcing reset (debugfs/gtM/force_reset_sync) shouldn't be used to idle GPU.
        pass

    # Create debugfs path to given parameter (without a base part):
    # gt@gt_num/[pf|vf@vf_num]/@attr
    # @vf_num: VF number (1-based) or 0 for PF
    # @gt_num: GT instance number
    # @subdir: subdirectory for attribute or empty string if not exists
    # @attr: iov parameter name
    # Returns: iov debugfs path to @attr
    def __helper_create_debugfs_path(self, vf_num: int, gt_num: int, subdir: str, attr: str) -> str:
        vf_gt_part = f'gt{gt_num}/pf' if vf_num == 0 else f'gt{gt_num}/vf{vf_num}'
        return f'{vf_gt_part}/{subdir}/{attr}'

    # PF spare resources
    # Debugfs location: [SRIOV debugfs base path]/gtM/pf/xxx_spare
    def get_pf_ggtt_spare(self, gt_num: int) -> int:
        path = self.__helper_create_debugfs_path(0, gt_num, '', 'ggtt_spare')
        return int(self.__read_debugfs(path))

    def set_pf_ggtt_spare(self, gt_num: int, val: int) -> None:
        path = self.__helper_create_debugfs_path(0, gt_num, '',  'ggtt_spare')
        self.__write_debugfs(path, str(val))

    def get_pf_lmem_spare(self, gt_num: int) -> int:
        path = self.__helper_create_debugfs_path(0, gt_num, '', 'lmem_spare')
        return int(self.__read_debugfs(path)) if self.has_lmem() else 0

    def set_pf_lmem_spare(self, gt_num: int, val: int) -> None:
        path = self.__helper_create_debugfs_path(0, gt_num, '', 'lmem_spare')
        if self.has_lmem():
            self.__write_debugfs(path, str(val))

    def get_pf_contexts_spare(self, gt_num: int) -> int:
        path = self.__helper_create_debugfs_path(0, gt_num, '', 'contexts_spare')
        return int(self.__read_debugfs(path))

    def set_pf_contexts_spare(self, gt_num: int, val: int) -> None:
        path = self.__helper_create_debugfs_path(0, gt_num, '', 'contexts_spare')
        self.__write_debugfs(path, str(val))

    def get_pf_doorbells_spare(self, gt_num: int) -> int:
        path = self.__helper_create_debugfs_path(0, gt_num, '', 'doorbells_spare')
        return int(self.__read_debugfs(path))

    def set_pf_doorbells_spare(self, gt_num: int, val: int) -> None:
        path = self.__helper_create_debugfs_path(0, gt_num, '', 'doorbells_spare')
        self.__write_debugfs(path, str(val))

    # PF specific provisioning parameters
    # Debugfs location: [SRIOV debugfs base path]/gtM/pf
    def get_pf_sched_priority(self, gt_num: int) -> SchedulingPriority:
        logger.warning("PF sched_priority param not available")
        return SchedulingPriority.LOW

    def set_pf_sched_priority(self, gt_num: int, val: SchedulingPriority) -> None:
        logger.warning("PF sched_priority param not available")

    def get_pf_policy_reset_engine(self, gt_num: int) -> int:
        path = self.__helper_create_debugfs_path(0, gt_num, '', 'reset_engine')
        return int(self.__read_debugfs(path))

    def set_pf_policy_reset_engine(self, gt_num: int, val: int) -> None:
        path = self.__helper_create_debugfs_path(0, gt_num, '', 'reset_engine')
        self.__write_debugfs(path, str(val))

    def get_pf_policy_sample_period_ms(self, gt_num: int) -> int:
        path = self.__helper_create_debugfs_path(0, gt_num, '', 'sample_period_ms')
        return int(self.__read_debugfs(path))

    def set_pf_policy_sample_period_ms(self, gt_num: int, val: int) -> None:
        path = self.__helper_create_debugfs_path(0, gt_num, '', 'sample_period_ms')
        self.__write_debugfs(path, str(val))

    def get_pf_policy_sched_if_idle(self, gt_num: int) -> int:
        path = self.__helper_create_debugfs_path(0, gt_num, '', 'sched_if_idle')
        return int(self.__read_debugfs(path))

    def set_pf_policy_sched_if_idle(self, gt_num: int, val: int) -> None:
        # In order to set strict scheduling policy, PF scheduling priority needs to be default
        path = self.__helper_create_debugfs_path(0, gt_num, '', 'sched_if_idle')
        self.__write_debugfs(path, str(val))

    # VF and PF provisioning parameters
    # Debugfs location: [SRIOV debugfs base path]/gtM/[pf|vfN]
    # @vf_num: VF number (1-based) or 0 for PF
    def get_ggtt_quota(self, vf_num: int, gt_num: int) -> int:
        if vf_num == 0:
            logger.warning("PF ggtt_quota not available")
            return 0

        path = self.__helper_create_debugfs_path(vf_num, gt_num, '', 'ggtt_quota')
        return int(self.__read_debugfs(path))

    def set_ggtt_quota(self, vf_num: int, gt_num: int, val: int) -> None:
        if vf_num == 0:
            logger.warning("PF ggtt_quota not available")
            return

        path = self.__helper_create_debugfs_path(vf_num, gt_num, '', 'ggtt_quota')
        self.__write_debugfs(path, str(val))

    def get_lmem_quota(self, vf_num: int, gt_num: int) -> int:
        if vf_num == 0:
            logger.warning("PF lmem_quota not available")
            return 0

        path = self.__helper_create_debugfs_path(vf_num, gt_num, '', 'lmem_quota')
        return int(self.__read_debugfs(path)) if self.has_lmem() else 0

    def set_lmem_quota(self, vf_num: int, gt_num: int, val: int) -> None:
        if vf_num == 0:
            logger.warning("PF lmem_quota not available")
            return

        path = self.__helper_create_debugfs_path(vf_num, gt_num, '', 'lmem_quota')
        if self.has_lmem():
            self.__write_debugfs(path, str(val))

    def get_contexts_quota(self, vf_num: int, gt_num: int) -> int:
        if vf_num == 0:
            logger.warning("PF contexts_quota not available")
            return 0

        path = self.__helper_create_debugfs_path(vf_num, gt_num, '', 'contexts_quota')
        return int(self.__read_debugfs(path))

    def set_contexts_quota(self, vf_num: int, gt_num: int, val: int) -> None:
        if vf_num == 0:
            logger.warning("PF contexts_quota not available")
            return

        path = self.__helper_create_debugfs_path(vf_num, gt_num, '', 'contexts_quota')
        self.__write_debugfs(path, str(val))

    def get_doorbells_quota(self, vf_num: int, gt_num: int) -> int:
        if vf_num == 0:
            logger.warning("PF doorbells_quota not available")
            return 0

        path = self.__helper_create_debugfs_path(vf_num, gt_num, '', 'doorbells_quota')
        return int(self.__read_debugfs(path))

    def set_doorbells_quota(self, vf_num: int, gt_num: int, val: int) -> None:
        if vf_num == 0:
            logger.warning("PF doorbells_quota not available")
            return

        path = self.__helper_create_debugfs_path(vf_num, gt_num, '', 'doorbells_quota')
        self.__write_debugfs(path, str(val))

    def get_exec_quantum_ms(self, vf_num: int, gt_num: int) -> int:
        path = self.__helper_create_debugfs_path(vf_num, gt_num, '', 'exec_quantum_ms')
        return int(self.__read_debugfs(path))

    def set_exec_quantum_ms(self, vf_num: int, gt_num: int, val: int) -> None:
        path = self.__helper_create_debugfs_path(vf_num, gt_num, '', 'exec_quantum_ms')
        self.__write_debugfs(path, str(val))

    def get_preempt_timeout_us(self, vf_num: int, gt_num: int) -> int:
        path = self.__helper_create_debugfs_path(vf_num, gt_num, '', 'preempt_timeout_us')
        return int(self.__read_debugfs(path))

    def set_preempt_timeout_us(self, vf_num: int, gt_num: int, val: int) -> None:
        path = self.__helper_create_debugfs_path(vf_num, gt_num, '', 'preempt_timeout_us')
        self.__write_debugfs(path, str(val))

    # Control state of the running VF (WO)
    # Debugfs location: [SRIOV debugfs base path]/gtM/vfN/control
    # Allows PF admin to pause, resume or stop handling
    # submission requests from given VF and clear provisioning.
    # control: "pause|resume|stop|clear"
    # For debug purposes only.
    def set_vf_control(self, vf_num: int, val: VfControl) -> None:
        path = self.__helper_create_debugfs_path(vf_num, 0, '', 'control')
        self.__write_debugfs(path, val)

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
