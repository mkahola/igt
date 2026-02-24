# SPDX-License-Identifier: MIT
# Copyright © 2024-2026 Intel Corporation

import base64
import json
import logging
import re
import shlex
import signal
import subprocess
import threading
import time
import typing
from pathlib import Path
from types import FrameType

from bench import exceptions
from bench.configurators.vmtb_config import VmtbIgtConfig
from bench.machines.machine_interface import (DEFAULT_TIMEOUT,
                                              MachineInterface, ProcessResult,
                                              SuspendMode)
from bench.machines.virtual.backends.guestagent import GuestAgentBackend
from bench.machines.virtual.backends.qmp_monitor import QmpMonitor
from bench.machines.virtual.device import VirtualDevice

logger = logging.getLogger('VirtualMachine')


class VirtualMachine(MachineInterface):
    class Decorators():
        @staticmethod
        def alarm_handler(sig: signal.Signals, tb: FrameType) -> typing.Any:
            raise exceptions.AlarmTimeoutError(f'Alarm timeout occured')

        @classmethod
        def timeout_signal(cls, func: typing.Callable) -> typing.Callable:
            def timeout_wrapper(*args: typing.Any, **kwargs: typing.Optional[typing.Any]) -> typing.Any:
                timeout: int = DEFAULT_TIMEOUT
                if len(args) > 2:
                    timeout = args[2] # Argument position in execute_wait(self, pid, timeout)
                elif kwargs.get('timeout') is not None:
                    if isinstance(kwargs['timeout'], int):
                        timeout = kwargs['timeout']

                # mypy: silence the following problem in signal.signal() call:
                # error: Argument 2 to "signal" has incompatible type "Callable[[Signals, FrameType], Any]";
                # expected "Union[Callable[[int, Optional[FrameType]], Any], int, Handlers, None]"  [arg-type]
                signal.signal(signal.SIGALRM, cls.alarm_handler) # type: ignore[arg-type]
                signal.alarm(timeout)
                try:
                    proc_ret = func(*args, **kwargs)
                except exceptions.AlarmTimeoutError:
                    logger.warning('Timeout (%ss) on %s', timeout, func.__name__)
                    raise
                finally:
                    signal.alarm(0)  # Cancel alarm

                return proc_ret

            return timeout_wrapper

    def __init__(self, vm_number: int, backing_image: str, driver: str,
                 igt_config: VmtbIgtConfig, vf_migration_support: bool) -> None:
        self.vmnum: int = vm_number
        self.drm_driver_name: str = driver
        self.igt_config: VmtbIgtConfig = igt_config
        self.vf_migration: bool = vf_migration_support

        # Passed VFs and VirtualDevices list - placeholder for multiple VFs passed to single VM:
        # currently only one VF/VirtualDevice per VM supported (ie. passed_vf_bdfs[0]).
        # TODO: add support for multiple VFs/VirtualDevices per VM
        self.passed_vf_bdfs: typing.List[str] = []
        self.gpu_devices: typing.List[VirtualDevice] = []
        self.process: typing.Optional[subprocess.Popen] = None

        self.questagent_sockpath = Path('/tmp') / f'qga{self.vmnum}.sock'
        self.qmp_sockpath = Path('/tmp') / f'mon{self.vmnum}.sock'

        if not Path(backing_image).exists():
            logger.error('No image for VM%s', self.vmnum)
            raise exceptions.GuestError(f'No image for VM{self.vmnum}')

        self.image: str = self.__create_qemu_image(backing_image)

        self.migrate_source_image: typing.Optional[str] = None
        self.migrate_destination_vm: bool = False

        self.dmesg_logger = self.__setup_dmesg_logger()

    def __str__(self) -> str:
        return f'VM{self.vmnum}-VF:{self.passed_vf_bdfs[0] if self.passed_vf_bdfs else 'N/A'}'

    def __del__(self) -> None:
        if not self.is_running():
            return

        # printing and not logging because loggers have some issues
        # in late deinitialization
        print(f'VM{self.vmnum} was not powered off')
        if not self.process:
            return
        self.process.terminate()
        # Lets wait and make sure that qemu shutdown
        try:
            self.process.communicate(timeout=30)
        except subprocess.TimeoutExpired:
            print('QEMU did not terminate, killing it')
            self.process.kill()

    def __setup_dmesg_logger(self) -> logging.Logger:
        """Configure VM dmesg logger.
        Logs are directed to dedicated vm[NUM]_dmesg.log and propagated to logfile.log.
        """
        dmesg_logger = logging.getLogger(f'VM{self.vmnum}-kmsg')
        # Remove any existing logger handlers to avoid duplicated prints for parametrized tests
        dmesg_logger.handlers.clear()
        dmesg_log_handler = logging.FileHandler(f'vm{self.vmnum}_dmesg.log')
        dmesg_logger.addHandler(dmesg_log_handler)

        return dmesg_logger

    def __get_backing_file_format(self, backing_file: str) -> typing.Any:
        """Get the format of the backing image file using qemu-img info."""
        command = ['qemu-img', 'info', '--output=json', backing_file]
        try:
            result = subprocess.run(command, capture_output=True, check=True)
            return json.loads(result.stdout)['format']
        except subprocess.CalledProcessError as exc:
            logger.error("Error executing qemu-img info: %s", exc.stderr)
            raise exceptions.GuestError(f'Error executing qemu-img info') from exc
        except json.JSONDecodeError as exc:
            logger.error("Invalid JSON output from qemu-img info: %s", exc)
            raise exceptions.GuestError('Invalid JSON output from qemu-img info') from exc

    def __create_qemu_image(self, backing_file: str) -> str:
        """Create a new qcow2 image with the specified backing file."""
        output_image = f'./vm{self.vmnum}_{time.time()}_image.qcow2'
        backing_format = self.__get_backing_file_format(backing_file)

        command = ['qemu-img', 'create',
                   '-f', 'qcow2', '-b', f'{backing_file}', '-F', f'{backing_format}', f'{output_image}']
        try:
            subprocess.run(command, check=True)
            logger.debug("[VM%s] Created image %s (backing file: %s, format: %s)",
                         self.vmnum, output_image, backing_file, backing_format)
        except subprocess.CalledProcessError as exc:
            logger.error('[VM%s] Error creating qcow2 image: %s', self.vmnum, exc)
            raise exceptions.GuestError('Error creating qcow2 image') from exc

        return output_image

    def __log_qemu_output(self, out: typing.TextIO) -> None:
        for line in iter(out.readline, ''):
            self.dmesg_logger.debug(line.strip())

    def __sockets_exists(self) -> bool:
        return self.questagent_sockpath.exists() and self.qmp_sockpath.exists()

    def __prepare_qemu_command(self) -> typing.List[str]:
        command = ['qemu-system-x86_64',
                   '-vnc', f':{self.vmnum}',
                   '-serial', 'stdio',
                   '-m', '4096',
                   '-vga', 'none',
                   '-net', 'nic',
                   '-net', f'user,hostfwd=tcp::{10000 + self.vmnum}-:22',
                   '-drive', f'file={self.image if not self.migrate_destination_vm else self.migrate_source_image}',
                   '-chardev', f'socket,path={self.questagent_sockpath},server=on,wait=off,id=qga{self.vmnum}',
                   '-device', 'virtio-serial',
                   '-device', f'virtserialport,chardev=qga{self.vmnum},name=org.qemu.guest_agent.0',
                   '-chardev', f'socket,id=mon{self.vmnum},path=/tmp/mon{self.vmnum}.sock,server=on,wait=off',
                   '-mon', f'chardev=mon{self.vmnum},mode=control']

        if self.passed_vf_bdfs:
            command.extend(['-enable-kvm', '-cpu', 'host', '-device', f'vfio-pci,host={self.passed_vf_bdfs[0]}'])
            if self.vf_migration:
                command[-1] += ',enable-migration=on'

        if self.migrate_destination_vm:
            # If VM is migration destination - run in stopped/prelaunch state (explicit resume required)
            command.extend(['-S'])

        logger.debug('QEMU command: %s', ' '.join(command))
        return command

    def __get_key(self, base: typing.Dict, path: typing.List[str]) -> typing.Any:
        cur = base
        for key in path:
            if cur is None or key not in cur:
                raise ValueError(f'The key {path} does not exist, aborting!')
            cur = cur[key]
        return cur

    def assign_vf(self, vf_bdf: str) -> None:
        """Pass VFs to VM - required to run QEMU (prior to VM power on)"""
        self.passed_vf_bdfs.append(vf_bdf)

    def set_migration_source(self, src_image: str) -> None:
        self.migrate_source_image = src_image
        self.migrate_destination_vm = True

    def get_drm_driver_name(self) -> str:
        return self.drm_driver_name

    def get_igt_config(self) -> VmtbIgtConfig:
        return self.igt_config

    def get_dut(self) -> VirtualDevice:
        # Currently only one first enumerated device is supported (virtual card0)
        return self.gpu_devices[0]

    def is_drm_driver_loaded(self) -> bool:
        return self.dir_exists(f'/sys/bus/pci/drivers/{self.drm_driver_name}')

    def load_drm_driver(self) -> None:
        """Load (modprobe) guest DRM driver."""
        if not self.is_drm_driver_loaded():
            logger.debug("VirtualMachine - load DRM driver")
            drv_probe_pid = self.execute(f'modprobe {self.drm_driver_name}')
            if self.execute_wait(drv_probe_pid).exit_code != 0:
                logger.error("%s driver probe failed on guest!", self.drm_driver_name)
                raise exceptions.GuestError(f'{self.drm_driver_name} driver probe failed on guest!')

    def unload_drm_driver(self) -> None:
        """Unload (remove) guest DRM driver."""
        logger.debug("VirtualMachine - unload DRM driver")
        for device in self.gpu_devices:
            logger.debug("Unbind %s from virtual device %s", self.drm_driver_name, device.pci_info.bdf)
            device.unbind_driver()

        rmmod_pid = self.execute(f'modprobe -rf {self.drm_driver_name}')
        if self.execute_wait(rmmod_pid).exit_code != 0:
            logger.error("DRM driver remove failed!")
            raise exceptions.HostError('DRM driver remove failed!')

        logger.debug("%s successfully removed", self.drm_driver_name)

    def discover_devices(self) -> None:
        """Detect all (virtual) PCI GPU devices on the guest and initialize VirtualDevice list."""
        if not self.is_drm_driver_loaded():
            logger.error("Unable to discover devices on guest - %s driver is not loaded!", self.drm_driver_name)
            raise exceptions.HostError(
                f'Unable to discover devices on guest - {self.drm_driver_name} driver is not loaded!')

        detected_devices: typing.List[VirtualDevice] = []
        drv_path = Path('/sys/bus/pci/drivers/') / self.drm_driver_name

        dev_dir_ls = self.dir_list(str(drv_path))

        # Look for a directory name with a PCI BDF (e.g. 0000:1a:00.0)
        for bdf in dev_dir_ls:
            match = re.match(r'\d{4}(?::[0-9a-z-A-Z]{2}){2}.[0-7]', bdf)
            if match:
                device = VirtualDevice(bdf, self)
                detected_devices.append(device)

        # Output list of detected devices sorted by an ascending card index (device minor number)
        self.gpu_devices = sorted(detected_devices, key=lambda dev: dev.pci_info.minor_number)

        if not self.gpu_devices:
            logger.error("Virtualized GPU PCI device (bound to %s driver) not detected!", self.drm_driver_name)
            raise exceptions.GuestError(
                f'Virtualized GPU PCI device (bound to {self.drm_driver_name} driver) not detected!')

        logger.debug("Detected virtualized GPU PCI device(s):")
        for dev in self.gpu_devices:
            logger.debug("[virtual card%s] PCI BDF: %s / DevID: %s (%s)",
                          dev.pci_info.minor_number, dev.pci_info.bdf, dev.pci_info.devid, dev.gpu_model)

    @Decorators.timeout_signal
    def poweron(self) -> None:
        logger.debug('Powering on VM%s', self.vmnum)
        if self.is_running():
            logger.warning('VM%s already running', self.vmnum)
            return

        command = self.__prepare_qemu_command()
        # We don't want to kill the process created here (like 'with' would do) so disable the following linter issue:
        # R1732: consider-using-with (Consider using 'with' for resource-allocating operations)
        # pylint: disable=R1732
        self.process = subprocess.Popen(
            args=command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            universal_newlines=True)

        qemu_stdout_log_thread = threading.Thread(
            target=self.__log_qemu_output, args=(
                self.process.stdout,), daemon=True)
        qemu_stdout_log_thread.start()

        qemu_stderr_log_thread = threading.Thread(
            target=self.__log_qemu_output, args=(
                self.process.stderr,), daemon=True)
        qemu_stderr_log_thread.start()

        if not self.is_running():
            logger.error('VM%s did not boot', self.vmnum)
            raise exceptions.GuestError(f'VM{self.vmnum} did not start')

        try:
            while not self.__sockets_exists():
                logger.info('waiting for socket')
                time.sleep(1)
            # Passing five minutes timeout for every command
            self.ga = GuestAgentBackend(str(self.questagent_sockpath), 300)
            self.qm = QmpMonitor(str(self.qmp_sockpath), 300)
            vm_status = self.qm.query_status()

            if not self.migrate_destination_vm and vm_status != 'running':
                self.process.terminate()
                logger.error('VM%s status not "running", instead: %s', self.vmnum, vm_status)
                raise exceptions.GuestError(f'VM{self.vmnum} status {vm_status}')
        except Exception as exc:
            logger.error('Error while booting VM%s: %s', self.vmnum, exc)
            self.process.terminate()
            raise exceptions.GuestError(f'VM{self.vmnum} crashed with {exc}') from exc

    def is_running(self) -> bool:
        if self.process is None:
            return False

        return_code = self.process.poll()
        if return_code is None:
            return True

        return False

    @Decorators.timeout_signal
    def poweroff(self) -> None:
        """Power off VM via the Guest-Agent guest-shutdown(powerdown) command."""
        logger.debug('Powering off VM%s', self.vmnum)
        assert self.process
        if not self.is_running():
            logger.warning('VM%s not running', self.vmnum)
            return

        try:
            self.ga.poweroff()
            # Wait for shutdown event
            event: str = self.qm.get_qmp_event()
            while event != 'SHUTDOWN':
                event = self.qm.get_qmp_event()
        except exceptions.AlarmTimeoutError:
            logger.warning('VM%s hanged on poweroff. Initiating forced termination', self.vmnum)
            self.process.terminate()
        finally:
            # Wait and make sure that qemu shutdown
            self.process.communicate()

            if self.__sockets_exists():
                # Remove leftovers and notify about unclear qemu shutdown
                self.questagent_sockpath.unlink()
                self.qmp_sockpath.unlink()
                logger.error('VM%s was not gracefully powered off - sockets exist', self.vmnum)
                raise exceptions.GuestError(f'VM{self.vmnum} was not gracefully powered off - sockets exist')

    def reboot(self) -> None:
        """Reboot VM via the Guest-Agent guest-shutdown(reboot) command."""
        logger.debug('Rebooting VM%s', self.vmnum)
        self.ga.reboot()

        # Wait for 2x RESET event (guest-reset)
        reset_event_count = 2
        while reset_event_count > 0:
            if self.qm.get_qmp_event() == 'RESET':
                reset_event_count -= 1

    def reset(self) -> None:
        """Reset VM via the QMP system_reset command."""
        logger.debug('Resetting VM%s', self.vmnum)
        self.qm.system_reset()

        # Wait for 2x RESET event (host-qmp-system-reset, guest-reset)
        reset_event_count = 2
        while reset_event_count > 0:
            if self.qm.get_qmp_event() == 'RESET':
                reset_event_count -= 1

    def pause(self) -> None:
        logger.debug('Pausing VM%s', self.vmnum)
        self.qm.stop()
        vm_status = self.qm.query_status()
        if vm_status != 'paused':
            if self.process:
                self.process.terminate()
            logger.error('VM%s status not "paused", instead: %s', self.vmnum, vm_status)
            raise exceptions.GuestError(f'VM{self.vmnum} status {vm_status}')

    def resume(self) -> None:
        logger.debug('Resuming VM%s', self.vmnum)
        self.qm.cont()
        vm_status = self.qm.query_status()
        if vm_status != 'running':
            if self.process:
                self.process.terminate()
            logger.error('VM%s status not "running", instead: %s', self.vmnum, vm_status)
            raise exceptions.GuestError(f'VM{self.vmnum} status {vm_status}')

    def quit(self) -> None:
        logger.debug('Quitting VM%s', self.vmnum)
        self.qm.quit()
        event: str = self.qm.get_qmp_event()
        while event != 'SHUTDOWN':
            event = self.qm.get_qmp_event()

    def _enable_suspend(self) -> None:
        if self.link_exists('/etc/systemd/system/suspend.target'):
            logger.debug('Enable (unmask) systemd suspend/sleep')
            self.execute('systemctl unmask suspend.target sleep.target')

    def suspend(self, mode: SuspendMode = SuspendMode.ACPI_S3) -> None:
        logger.debug('Suspending VM%s (mode: %s)', self.vmnum, mode)
        self._enable_suspend()
        if mode == SuspendMode.ACPI_S3:
            self.ga.suspend_ram()
        elif mode == SuspendMode.ACPI_S4:
            # self.ga.suspend_disk()
            raise exceptions.GuestError('Guest S4 support not implemented')
        else:
            raise exceptions.GuestError('Unknown suspend mode')

        event: str = self.qm.get_qmp_event()
        while event != 'SUSPEND':
            event = self.qm.get_qmp_event()

        vm_status = self.qm.query_status()
        if vm_status != 'suspended':
            if self.process:
                self.process.terminate()
            logger.error('VM%s status not "suspended", instead: %s', self.vmnum, vm_status)
            raise exceptions.GuestError(f'VM{self.vmnum} status {vm_status}')

    def wakeup(self) -> None:
        logger.debug('Waking up VM%s', self.vmnum)
        self.qm.system_wakeup()

        event: str = self.qm.get_qmp_event()
        while event != 'WAKEUP':
            event = self.qm.get_qmp_event()

        vm_status = self.qm.query_status()
        if vm_status != 'running':
            if self.process:
                self.process.terminate()
            logger.error('VM%s status not "running", instead: %s', self.vmnum, vm_status)
            raise exceptions.GuestError(f'VM{self.vmnum} status {vm_status}')

    # {"execute": "guest-exec", "arguments":{"path": "/some/path", "arg": [], "capture-output": true}}
    # {"error": {"class": "GenericError", "desc": "Guest... "}}
    def execute(self, command: str) -> int:
        arr_cmd = shlex.split(command)
        execout: typing.Dict = self.ga.execute(arr_cmd[0], arr_cmd[1:])
        ret = execout.get('return')
        if ret:
            pid: int = ret.get('pid')
            logger.debug("Run command on VM%s: %s (PID: %s)", self.vmnum, command, pid)
            return pid

        logger.error('Command %s did not return pid', command)
        raise exceptions.GuestError(f'No pid returned: {execout}')

    # {'error': {'class': 'GenericError', 'desc': "Invalid parameter 'pid'"}}
    def execute_status(self, pid: int) -> ProcessResult:
        out = self.ga.execute_status(pid)
        status = out.get('return')
        if not status:
            raise exceptions.GuestError(f'Not output from guest agent: {out}')

        b64stdout = status.get('out-data', '')
        stdout = base64.b64decode(b64stdout).decode('utf-8')

        b64stderr = status.get('err-data', '')
        stderr = base64.b64decode(b64stderr).decode('utf-8')

        return ProcessResult(status.get('exited'), status.get('exitcode', None), stdout, stderr)

    @Decorators.timeout_signal
    def execute_wait(self, pid: int, timeout: int = DEFAULT_TIMEOUT) -> ProcessResult:
        exec_status = ProcessResult(False, -1, '', '')
        while not exec_status.exited:
            exec_status = self.execute_status(pid)
            time.sleep(1)

        return exec_status

    def execute_signal(self, pid: int, sig: signal.Signals) -> None:
        signum = int(sig)
        killpid = self.execute(f'kill -{signum} {pid}')
        self.execute_wait(killpid)

    def read_file_content(self, path: str) -> str:
        out = self.ga.guest_file_open(path, 'r')
        handle = out.get('return')
        if not handle:
            raise exceptions.GuestError('Could not open file on guest')

        try:
            eof: bool = False
            file_content: typing.List[str] = []
            while not eof:
                ret = self.ga.guest_file_read(handle)
                eof = self.__get_key(ret, ['return', 'eof'])
                b64buf: str = self.__get_key(ret, ['return', 'buf-b64'])
                file_content.append(base64.b64decode(b64buf).decode('utf-8'))
        finally:
            self.ga.guest_file_close(handle)

        return ''.join(file_content)

    def write_file_content(self, path: str, content: str) -> int:
        out: typing.Dict = self.ga.guest_file_open(path, 'w')
        handle = out.get('return')
        if not handle:
            raise exceptions.GuestError('Could not open file on guest')

        b64buf: bytes = base64.b64encode(content.encode())

        try:
            ret = self.ga.guest_file_write(handle, b64buf.decode('utf-8'))
            count: int = self.__get_key(ret, ['return', 'count'])
        finally:
            self.ga.guest_file_close(handle)

        return count

    def dir_exists(self, path: str) -> bool:
        pid = self.execute(f'/bin/sh -c "[ -d {path} ]"')
        status = self.execute_wait(pid)
        if status.exit_code:
            return False
        return True

    def dir_list(self, path: str) -> typing.List[str]:
        pid = self.execute(f'/bin/sh -c "ls {path}"')
        status: ProcessResult = self.execute_wait(pid)
        if status.exit_code:
            raise exceptions.GuestError(f'VM ls failed - error: {status.exit_code}')

        return status.stdout.split()

    def link_exists(self, path: str) -> bool:
        pid = self.execute(f'/bin/sh -c "[ -h {path} ]"')
        status = self.execute_wait(pid)
        if status.exit_code:
            return False
        return True

    @Decorators.timeout_signal
    def ping(self, timeout: int = DEFAULT_TIMEOUT) -> bool:
        """Ping guest and return true if responding, false otherwise."""
        logger.debug('Ping VM%s', self.vmnum)
        try:
            self.ga.ping()
        except exceptions.AlarmTimeoutError:
            logger.warning('VM%s not responded to ping', self.vmnum)
            return False

        return True

    @Decorators.timeout_signal
    def save_state(self) -> None:
        logger.debug('Saving VM%s state (snapshot)', self.vmnum)
        self.qm.save_snapshot()

        job_status: str = self.qm.get_qmp_event_job()
        while job_status != 'concluded':
            job_status = self.qm.get_qmp_event_job()

        job_status, job_error = self.qm.query_jobs('snapshot-save')
        if job_status == 'concluded' and job_error is not None:
            raise exceptions.GuestError(f'VM{self.vmnum} state save error: {job_error}')

        logger.debug('VM%s state save finished successfully', self.vmnum)

    @Decorators.timeout_signal
    def load_state(self) -> None:
        logger.debug('Loading VM state (snapshot)')
        self.qm.load_snapshot()

        job_status: str = self.qm.get_qmp_event_job()
        while job_status != 'concluded':
            job_status = self.qm.get_qmp_event_job()

        job_status, job_error = self.qm.query_jobs('snapshot-load')
        if job_status == 'concluded' and job_error is not None:
            raise exceptions.GuestError(f'VM{self.vmnum} state load error: {job_error}')

        logger.debug('VM state load finished successfully')
