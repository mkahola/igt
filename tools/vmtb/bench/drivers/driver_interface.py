# SPDX-License-Identifier: MIT
# Copyright © 2024-2026 Intel Corporation

import abc
import enum
import logging
import typing
from pathlib import Path

from bench import exceptions
from bench.machines.machine_interface import MachineInterface
from bench.helpers.log import LogDecorators

logger = logging.getLogger('DriverInterface')


class SchedulingPriority(str, enum.Enum):
    LOW = 'low'
    NORMAL = 'normal'
    HIGH = 'high'


class VfControl(str, enum.Enum):
    pause = 'pause'
    resume = 'resume'
    stop = 'stop'
    clear = 'clear'

    def __str__(self) -> str:
        return str.__str__(self)


class DriverInterface(abc.ABC):
    """Base class for DRM drivers (Physical and Virtual).
    Provide common operations for all drivers like bind/unbind, reset etc.
    """
    def __init__(self, bdf: str) -> None:
        self.pci_bdf = bdf
        self.sysfs_device_path = Path('/sys/bus/pci/devices') / self.pci_bdf
        self.sysfs_driver_path = Path('/sys/bus/pci/drivers') / self.get_name()
        self.debugfs_path = Path('/sys/kernel/debug/dri') / self.pci_bdf

    @abc.abstractmethod
    def get_name(self) -> str:
        raise NotImplementedError

    @abc.abstractmethod
    def write_sysfs(self, path: Path, value: str) -> None:
        raise NotImplementedError

    @abc.abstractmethod
    def read_sysfs(self, path: Path) -> str:
        raise NotImplementedError

    @abc.abstractmethod
    def write_debugfs(self, file: str, value: str) -> None:
        raise NotImplementedError

    @abc.abstractmethod
    def read_debugfs(self, file: str) -> str:
        raise NotImplementedError

    @LogDecorators.parse_kmsg
    def bind(self) -> None:
        self.write_sysfs((self.sysfs_driver_path / 'bind'), self.pci_bdf)

    @LogDecorators.parse_kmsg
    def unbind(self) -> None:
        self.write_sysfs((self.sysfs_driver_path / 'unbind'), self.pci_bdf)

    @LogDecorators.parse_kmsg
    def flr(self) -> None:
        self.write_sysfs((self.sysfs_device_path / 'reset'), '1')


class PfDriverInterface(DriverInterface, abc.ABC):
    """Base class for PF drivers, extends common DriverInterface base class.
    Provide operations specific for PF drivers like read/write sysfs on Host,
    set number of VFs to enable, set/get provisioning related attributes etc.
    """
    @LogDecorators.parse_kmsg
    def __write_fs(self, path: Path, value: str) -> None:
        try:
            path.write_text(value)
            logger.debug("Write: %s -> %s", value, path)
        except Exception as exc:
            logger.error("Unable to write %s -> %s", value, path)
            raise exceptions.HostError(f'Could not write to {path}. Error: {exc}') from exc

    @LogDecorators.parse_kmsg
    def __read_fs(self, path: Path) -> str:
        try:
            ret = path.read_text()
        except Exception as exc:
            logger.error("Unable to read %s", path)
            raise exceptions.HostError(f'Could not read from {path}. Error: {exc}') from exc

        logger.debug("Read: %s -> %s", path, ret.strip())
        return ret

    def write_sysfs(self, path: Path, value: str) -> None:
        self.__write_fs(path, value)

    def read_sysfs(self, path: Path) -> str:
        return str(self.__read_fs(path))

    def write_debugfs(self, file: str, value: str) -> None:
        self.__write_fs(self.debugfs_path / file, value)

    def read_debugfs(self, file: str) -> str:
        return str(self.__read_fs(self.debugfs_path / file))

    @abc.abstractmethod
    def get_totalvfs(self) -> int:
        raise NotImplementedError

    @abc.abstractmethod
    def get_numvfs(self) -> int:
        raise NotImplementedError

    @abc.abstractmethod
    def set_numvfs(self, val: int) -> None:
        raise NotImplementedError

    @abc.abstractmethod
    def get_drivers_autoprobe(self) -> int:
        raise NotImplementedError

    @abc.abstractmethod
    def set_drivers_autoprobe(self, val: int) -> None:
        raise NotImplementedError

    @abc.abstractmethod
    def get_num_gts(self) -> int:
        raise NotImplementedError

    @abc.abstractmethod
    def has_lmem(self) -> bool:
        raise NotImplementedError

    @abc.abstractmethod
    def is_media_gt(self, gt_num: int) -> bool:
        raise NotImplementedError

    @abc.abstractmethod
    def restore_auto_provisioning(self) -> None:
        raise NotImplementedError

    @abc.abstractmethod
    def cancel_work(self) -> None:
        raise NotImplementedError

    # PF provisioning
    @abc.abstractmethod
    def get_pf_ggtt_spare(self, gt_num: int) -> int:
        raise NotImplementedError

    @abc.abstractmethod
    def set_pf_ggtt_spare(self, gt_num: int, val: int) -> None:
        raise NotImplementedError

    @abc.abstractmethod
    def get_pf_lmem_spare(self, gt_num: int) -> int:
        raise NotImplementedError

    @abc.abstractmethod
    def set_pf_lmem_spare(self, gt_num: int, val: int) -> None:
        raise NotImplementedError

    @abc.abstractmethod
    def get_pf_contexts_spare(self, gt_num: int) -> int:
        raise NotImplementedError

    @abc.abstractmethod
    def set_pf_contexts_spare(self, gt_num: int, val: int) -> None:
        raise NotImplementedError

    @abc.abstractmethod
    def get_pf_doorbells_spare(self, gt_num: int) -> int:
        raise NotImplementedError

    @abc.abstractmethod
    def set_pf_doorbells_spare(self, gt_num: int, val: int) -> None:
        raise NotImplementedError

    @abc.abstractmethod
    def get_pf_policy_reset_engine(self, gt_num: int) -> int:
        raise NotImplementedError

    @abc.abstractmethod
    def set_pf_policy_reset_engine(self, gt_num: int, val: int) -> None:
        raise NotImplementedError

    @abc.abstractmethod
    def get_pf_policy_sample_period_ms(self, gt_num: int) -> int:
        raise NotImplementedError

    @abc.abstractmethod
    def set_pf_policy_sample_period_ms(self, gt_num: int, val: int) -> None:
        raise NotImplementedError

    # VF provisioning
    @abc.abstractmethod
    def get_ggtt_quota(self, vf_num: int, gt_num: int) -> int:
        raise NotImplementedError

    @abc.abstractmethod
    def set_ggtt_quota(self, vf_num: int, gt_num: int, val: int) -> None:
        raise NotImplementedError

    @abc.abstractmethod
    def get_lmem_quota(self, vf_num: int, gt_num: int) -> int:
        raise NotImplementedError

    @abc.abstractmethod
    def set_lmem_quota(self, vf_num: int, gt_num: int, val: int) -> None:
        raise NotImplementedError

    @abc.abstractmethod
    def get_contexts_quota(self, vf_num: int, gt_num: int) -> int:
        raise NotImplementedError

    @abc.abstractmethod
    def set_contexts_quota(self, vf_num: int, gt_num: int, val: int) -> None:
        raise NotImplementedError

    @abc.abstractmethod
    def get_doorbells_quota(self, vf_num: int, gt_num: int) -> int:
        raise NotImplementedError

    @abc.abstractmethod
    def set_doorbells_quota(self, vf_num: int, gt_num: int, val: int) -> None:
        raise NotImplementedError

    # Scheduling provisioning
    @abc.abstractmethod
    def get_exec_quantum_ms(self, fn_num: int) -> int:
        "Get execution quantum (ms) for PF/VF (EQ/exec_quantum_ms)."
        raise NotImplementedError

    @abc.abstractmethod
    def set_exec_quantum_ms(self, fn_num: int, val: int) -> None:
        "Set execution quantum (ms) for PF/VF (EQ/exec_quantum_ms)."
        raise NotImplementedError

    @abc.abstractmethod
    def set_bulk_exec_quantum_ms(self, val: int) -> None:
        "Set execution quantum (ms) for PF and all VFs (EQ/exec_quantum_ms)."
        raise NotImplementedError

    @abc.abstractmethod
    def get_preempt_timeout_us(self, fn_num: int) -> int:
        "Get preemption timeout (us) for PF/VF (PT/preempt_timeout_us)."
        raise NotImplementedError

    @abc.abstractmethod
    def set_preempt_timeout_us(self, fn_num: int, val: int) -> None:
        "Set preemption timeout (us) for PF/VF (PT/preempt_timeout_us)."
        raise NotImplementedError

    @abc.abstractmethod
    def set_bulk_preempt_timeout_us(self, val: int) -> None:
        "Set preemption timeout (us) for PF and all VFs (PT/preempt_timeout_us)."
        raise NotImplementedError

    @abc.abstractmethod
    def get_sched_priority(self, fn_num: int) -> SchedulingPriority:
        "Get scheduling priority (sched_priority) of PF/VF: low, normal or high."
        raise NotImplementedError

    @abc.abstractmethod
    def set_pf_sched_priority(self, val: SchedulingPriority) -> None:
        "Set scheduling priority (sched_priority) for PF only."
        # Independent sched_prio setting is available only for PF
        raise NotImplementedError

    @abc.abstractmethod
    def set_bulk_sched_priority(self, val: SchedulingPriority) -> None:
        "Set scheduling priority (sched_priority) for PF and all VFs."
        # Set sched_prio for PF and all VFs.
        # Setting sched_priority for a single VF independently is not supported currently.
        raise NotImplementedError

    @abc.abstractmethod
    def set_vf_control(self, vf_num: int, val: VfControl) -> None:
        raise NotImplementedError

    @abc.abstractmethod
    def get_ggtt_available(self, gt_num: int) -> typing.Tuple[int, int]:
        raise NotImplementedError


class VfDriver(DriverInterface):
    """Base class for VF drivers, extends common DriverInterface base class.
    Provide operations specific for VF drivers like read/write sysfs on Guest/VM.
    """
    def __init__(self, bdf: str, vm: MachineInterface) -> None:
        # VirtualMachine instance is required for VM filesystem access via QEMU Guest-Agent
        self.vm: MachineInterface = vm
        super().__init__(bdf)

    def get_name(self) -> str:
        return self.vm.get_drm_driver_name()

    def write_sysfs(self, path: Path, value: str) -> None:
        self.vm.write_file_content(str(path), value)

    def read_sysfs(self, path: Path) -> str:
        return self.vm.read_file_content(str(path))

    def write_debugfs(self, file: str, value: str) -> None:
        self.vm.write_file_content(str(self.debugfs_path / file), value)

    def read_debugfs(self, file: str) -> str:
        return self.vm.read_file_content(str(self.debugfs_path / file))
