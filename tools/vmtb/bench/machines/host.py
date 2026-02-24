# SPDX-License-Identifier: MIT
# Copyright © 2024-2026 Intel Corporation

import logging
import shlex
import signal
import subprocess
import typing
from pathlib import Path

from bench import exceptions
from bench.configurators.vmtb_config import VmtbIgtConfig
from bench.helpers.log import LogDecorators
from bench.machines.machine_interface import (DEFAULT_TIMEOUT,
                                              MachineInterface, ProcessResult,
                                              SuspendMode)
from bench.machines.physical.device import Device

logger = logging.getLogger('Host')


class Host(MachineInterface):
    def __init__(self) -> None:
        self.running_procs: typing.Dict[int, subprocess.Popen] = {}
        self.gpu_devices: typing.List[Device] = []
        # Initialize in conftest/VmmTestingSetup:
        self.drm_driver_name: str
        self.igt_config: VmtbIgtConfig

    def __str__(self) -> str:
        return 'Host'

    @LogDecorators.parse_kmsg
    def execute(self, command: str) -> int:
        cmd_arr = shlex.split(command)
        # We don't want to kill the process created here (like 'with' would do) so disable the following linter issue:
        # R1732: consider-using-with (Consider using 'with' for resource-allocating operations)
        # pylint: disable=R1732
        # TODO: but maybe 'subprocess.run' function would fit instead of Popen constructor?
        process = subprocess.Popen(cmd_arr,
                                   stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE,
                                   universal_newlines=True)

        self.running_procs[process.pid] = process
        logger.debug("Run command: %s (PID: %s)", command, process.pid)
        return process.pid

    @LogDecorators.parse_kmsg
    def execute_status(self, pid: int) -> ProcessResult:
        proc = self.running_procs.get(pid, None)
        if not proc:
            logger.error("No process with PID: %s", pid)
            raise exceptions.HostError(f'No process with PID: {pid}')

        exit_code: typing.Optional[int] = proc.poll()
        logger.debug("PID %s -> exit code %s", pid, exit_code)
        if exit_code is None:
            return ProcessResult(False, exit_code, '', '')

        out, err = proc.communicate()
        return ProcessResult(True, exit_code, out, err)

    @LogDecorators.parse_kmsg
    def execute_wait(self, pid: int, timeout: int = DEFAULT_TIMEOUT) -> ProcessResult:
        proc = self.running_procs.get(pid, None)
        if not proc:
            logger.error("No process with PID: %s", pid)
            raise exceptions.HostError(f'No process with PID: {pid}')

        out = ''
        err = ''
        try:
            out, err = proc.communicate(timeout)
        except subprocess.TimeoutExpired as exc:
            logger.warning("Timeout (%ss) expired for PID: %s", exc.timeout, pid)
            raise

        return ProcessResult(True, proc.poll(), out, err)

    @LogDecorators.parse_kmsg
    def execute_signal(self, pid: int, sig: signal.Signals) -> None:
        proc = self.running_procs.get(pid, None)
        if not proc:
            logger.error("No process with PID: %s", pid)
            raise exceptions.HostError(f'No process with PID: {pid}')

        proc.send_signal(sig)

    def read_file_content(self, path: str) -> str:
        with open(path, encoding='utf-8') as f:
            content = f.read()
        return content

    def write_file_content(self, path: str, content: str) -> int:
        with open(path, 'w', encoding='utf-8') as f:
            return f.write(content)

    def dir_exists(self, path: str) -> bool:
        return Path(path).is_dir()

    def get_drm_driver_name(self) -> str:
        # Used as a part of MachineInterface for helpers
        return self.drm_driver_name

    def get_igt_config(self) -> VmtbIgtConfig:
        # Used as a part of MachineInterface to initialize IgtExecutor
        return self.igt_config

    def is_driver_loaded(self, driver_name: str) -> bool:
        driver_path = Path('/sys/bus/pci/drivers/') / driver_name
        return driver_path.exists()

    def is_driver_available(self, driver_name: str) -> bool:
        modinfo_pid = self.execute(f'modinfo -F filename {driver_name}')
        modinfo_result: ProcessResult = self.execute_wait(modinfo_pid)
        return modinfo_result.exit_code == 0

    def load_drivers(self) -> None:
        """Load (modprobe) required host drivers (DRM and VFIO)."""
        drivers_to_probe = [self.drm_driver_name, f'{self.drm_driver_name}-vfio-pci']
        # If vendor specific VFIO (ex. xe-vfio-pci) is not present, probe a regular vfio-pci
        if not self.is_driver_available(drivers_to_probe[1]):
            logger.warning("VFIO driver: '%s' is not available - use 'vfio-pci'", drivers_to_probe[1])
            drivers_to_probe[1] = 'vfio-pci'

        for driver in drivers_to_probe:
            if not self.is_driver_loaded(driver):
                logger.info("%s driver is not loaded - probe module", driver)
                drv_probe_pid = self.execute(f'modprobe {driver}')
                if self.execute_wait(drv_probe_pid).exit_code != 0:
                    logger.error("%s driver probe failed!", driver)
                    raise exceptions.HostError(f'{driver} driver probe failed!')

    def unload_drivers(self) -> None:
        """Unload (remove) host drivers (DRM and VFIO)."""
        logger.debug("Cleanup - unload drivers\n")
        vfio_driver = f'{self.drm_driver_name}-vfio-pci'
        if not self.is_driver_loaded(vfio_driver):
            vfio_driver = 'vfio-pci'

        rmmod_pid = self.execute(f'modprobe -rf {vfio_driver}')
        if self.execute_wait(rmmod_pid).exit_code != 0:
            logger.error("VFIO driver remove failed!")
            raise exceptions.HostError('VFIO driver remove failed!')

        for device in self.gpu_devices:
            logger.debug("Unbind %s from device %s", self.drm_driver_name, device.pci_info.bdf)
            device.unbind_driver()

        rmmod_pid = self.execute(f'modprobe -rf {self.drm_driver_name}')
        if self.execute_wait(rmmod_pid).exit_code != 0:
            logger.error("DRM driver remove failed!")
            raise exceptions.HostError('DRM driver remove failed!')

        logger.debug("%s/%s successfully removed", self.drm_driver_name, vfio_driver)

    def discover_devices(self) -> None:
        """Detect all PCI GPU devices on the host and initialize Device list."""
        if not self.is_driver_loaded(self.drm_driver_name):
            logger.error("Unable to discover devices - %s driver is not loaded!", self.drm_driver_name)
            raise exceptions.HostError(f'Unable to discover devices - {self.drm_driver_name} driver is not loaded!')

        detected_devices: typing.List[Device] = []
        drv_path = Path('/sys/bus/pci/drivers/') / self.drm_driver_name

        # Look for a directory name with a PCI BDF (e.g. 0000:1a:00.0)
        for dev_bdf_dir in drv_path.glob('*:*:*.[0-7]'):
            bdf = dev_bdf_dir.name
            device = Device(bdf, self.drm_driver_name)
            detected_devices.append(device)

        # Output list of detected devices sorted by an ascending card index (device minor number)
        self.gpu_devices = sorted(detected_devices, key=lambda dev: dev.pci_info.minor_number)

        if not self.gpu_devices:
            logger.error("GPU PCI device (bound to %s driver) not detected!", self.drm_driver_name)
            raise exceptions.HostError(f'GPU PCI device (bound to {self.drm_driver_name} driver) not detected!')

        logger.debug("Detected GPU PCI device(s):")
        for dev in self.gpu_devices:
            logger.debug("[card%s] PCI BDF: %s / DevID: %s (%s)",
                          dev.pci_info.minor_number, dev.pci_info.bdf, dev.pci_info.devid, dev.gpu_model)

    def get_device(self, dev_minor: int) -> typing.Optional[Device]:
        """Find device with a given minor number within detected GPU devices list."""
        return next((dev for dev in self.gpu_devices if dev.pci_info.minor_number == dev_minor), None)

    def suspend(self, mode: SuspendMode = SuspendMode.ACPI_S3) -> None:
        """Perform host suspend cycle (ACPI S3) via rtcwake tool."""
        wakeup_delay = 10 # wakeup timer in seconds
        logger.debug("Suspend-resume via rtcwake (mode: %s, wakeup delay: %ss)", mode, wakeup_delay)

        suspend_pid = self.execute(f'rtcwake -s {wakeup_delay} -m {mode}')
        suspend_result: ProcessResult = self.execute_wait(suspend_pid)
        if suspend_result.exit_code != 0:
            logger.error("Suspend failed - error: %s", suspend_result.stderr)
            raise exceptions.HostError(f'Suspend failed - error: {suspend_result.stderr}')
