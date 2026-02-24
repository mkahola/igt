# SPDX-License-Identifier: MIT
# Copyright © 2024-2026 Intel Corporation

import abc
import enum
import typing


class SchedulingPriority(enum.Enum):
    LOW = 0
    NORMAL = 1
    HIGH = 2


class VfControl(str, enum.Enum):
    pause = 'pause'
    resume = 'resume'
    stop = 'stop'
    clear = 'clear'

    def __str__(self) -> str:
        return str.__str__(self)


class DriverInterface(abc.ABC):

    @staticmethod
    @abc.abstractmethod
    def get_name() -> str:
        raise NotImplementedError

    @abc.abstractmethod
    def bind(self, bdf: str) -> None:
        raise NotImplementedError

    @abc.abstractmethod
    def unbind(self, bdf: str) -> None:
        raise NotImplementedError

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
    def get_auto_provisioning(self) -> bool:
        raise NotImplementedError

    @abc.abstractmethod
    def set_auto_provisioning(self, val: bool) -> None:
        raise NotImplementedError

    @abc.abstractmethod
    def cancel_work(self) -> None:
        raise NotImplementedError

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
    def get_pf_sched_priority(self, gt_num: int) -> SchedulingPriority:
        raise NotImplementedError

    @abc.abstractmethod
    def set_pf_sched_priority(self, gt_num: int, val: SchedulingPriority) -> None:
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

    @abc.abstractmethod
    def get_pf_policy_sched_if_idle(self, gt_num: int) -> int:
        raise NotImplementedError

    @abc.abstractmethod
    def set_pf_policy_sched_if_idle(self, gt_num: int, val: int) -> None:
        raise NotImplementedError

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

    @abc.abstractmethod
    def get_exec_quantum_ms(self, vf_num: int, gt_num: int) -> int:
        raise NotImplementedError

    @abc.abstractmethod
    def set_exec_quantum_ms(self, vf_num: int, gt_num: int, val: int) -> None:
        raise NotImplementedError

    @abc.abstractmethod
    def get_preempt_timeout_us(self, vf_num: int, gt_num: int) -> int:
        raise NotImplementedError

    @abc.abstractmethod
    def set_preempt_timeout_us(self, vf_num: int, gt_num: int, val: int) -> None:
        raise NotImplementedError

    @abc.abstractmethod
    def set_vf_control(self, vf_num: int, val: VfControl) -> None:
        raise NotImplementedError

    @abc.abstractmethod
    def get_ggtt_available(self, gt_num: int) -> typing.Tuple[int, int]:
        raise NotImplementedError
