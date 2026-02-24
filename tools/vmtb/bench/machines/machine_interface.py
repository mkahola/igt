# SPDX-License-Identifier: MIT
# Copyright © 2024-2026 Intel Corporation

import abc
import enum
import signal
import typing

from bench.configurators.vmtb_config import VmtbIgtConfig

DEFAULT_TIMEOUT: int = 1200 # Default machine execution wait timeout in seconds


class ProcessResult(typing.NamedTuple):
    exited: bool = False
    exit_code: typing.Optional[int] = None
    stdout: str = ''
    stderr: str = ''


class SuspendMode(str, enum.Enum):
    ACPI_S3 = 'mem'    # Suspend to RAM aka sleep
    ACPI_S4 = 'disk'   # Suspend to disk aka hibernation

    def __str__(self) -> str:
        return str.__str__(self)


class MachineInterface(metaclass=abc.ABCMeta):

    @abc.abstractmethod
    def execute(self, command: str) -> int:
        raise NotImplementedError

    @abc.abstractmethod
    def execute_status(self, pid: int) -> ProcessResult:
        raise NotImplementedError

    @abc.abstractmethod
    def execute_wait(self, pid: int, timeout: int) -> ProcessResult:
        raise NotImplementedError

    @abc.abstractmethod
    def execute_signal(self, pid: int, sig: signal.Signals) -> None:
        raise NotImplementedError

    @abc.abstractmethod
    def read_file_content(self, path: str) -> str:
        raise NotImplementedError

    @abc.abstractmethod
    def write_file_content(self, path: str, content: str) -> int:
        raise NotImplementedError

    @abc.abstractmethod
    def dir_exists(self, path: str) -> bool:
        raise NotImplementedError

    @abc.abstractmethod
    def dir_list(self, path: str) -> typing.List[str]:
        raise NotImplementedError

    @abc.abstractmethod
    def get_drm_driver_name(self) -> str:
        raise NotImplementedError

    @abc.abstractmethod
    def get_igt_config(self) -> VmtbIgtConfig:
        raise NotImplementedError
