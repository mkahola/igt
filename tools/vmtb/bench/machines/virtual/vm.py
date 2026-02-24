# SPDX-License-Identifier: MIT
# Copyright © 2024-2026 Intel Corporation

import base64
import json
import logging
import os
import posixpath
import shlex
import signal
import subprocess
import threading
import time
import typing
from types import FrameType

from bench import exceptions
from bench.configurators.vmtb_config import VmtbIgtConfig
from bench.machines.machine_interface import (DEFAULT_TIMEOUT,
                                              MachineInterface, ProcessResult,
                                              SuspendMode)
from bench.machines.virtual.backends.guestagent import GuestAgentBackend
from bench.machines.virtual.backends.qmp_monitor import QmpMonitor

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
        self.vf_bdf: typing.Optional[str] = None
        self.process: typing.Optional[subprocess.Popen] = None
        self.vmnum: int = vm_number
        self.card_num: int = 0
        self.sysfs_prefix_path = posixpath.join('/sys/class/drm/', f'card{str(self.card_num)}')
        self.questagent_sockpath = posixpath.join('/tmp', f'qga{self.vmnum}.sock')
        self.qmp_sockpath = posixpath.join('/tmp', f'mon{self.vmnum}.sock')
        self.drm_driver_name: str = driver
        self.igt_config: VmtbIgtConfig = igt_config
        self.vf_migration: bool = vf_migration_support

        if not posixpath.exists(backing_image):
            logger.error('No image for VM%s', self.vmnum)
            raise exceptions.GuestError(f'No image for VM{self.vmnum}')
        self.image: str = self.__create_qemu_image(backing_image)
        self.migrate_source_image: typing.Optional[str] = None
        self.migrate_destination_vm: bool = False

        # Resources provisioned to the VF/VM:
        self._lmem_size: typing.Optional[int] = None
        self._ggtt_size: typing.Optional[int] = None
        self._contexts: typing.Optional[int] = None
        self._doorbells: typing.Optional[int] = None

        # GT number and tile is relevant mainly for multi-tile devices
        # List of all GTs used by a given VF:
        # - for single-tile: only root [0]
        # - for multi-tile Mode 2/3: either root [0] or remote [1]
        # - for multi-tile Mode 1: spans on both tiles [0, 1]
        self._gt_nums: typing.List[int] = []
        self._tile_mask: typing.Optional[int] = None

    def __str__(self) -> str:
        return f'VM{self.vmnum}_{self.vf_bdf}'

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
        stdoutlog = logging.getLogger(f'VM{self.vmnum}-kmsg')
        for line in iter(out.readline, ''):
            stdoutlog.debug(line.strip())

    def __sockets_exists(self) -> bool:
        return os.path.exists(self.questagent_sockpath) and os.path.exists(self.qmp_sockpath)

    def __get_popen_command(self) -> typing.List[str]:
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

        if self.vf_bdf:
            command.extend(['-enable-kvm', '-cpu', 'host', '-device', f'vfio-pci,host={self.vf_bdf}'])
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

    @property
    def get_vm_num(self) -> int:
        return self.vmnum

    def assign_vf(self, vf_bdf: str) -> None:
        self.vf_bdf = vf_bdf

    def set_migration_source(self, src_image: str) -> None:
        self.migrate_source_image = src_image
        self.migrate_destination_vm = True

    @property
    def lmem_size(self) -> typing.Optional[int]:
        if self._lmem_size is None:
            self.helper_get_debugfs_selfconfig()

        return self._lmem_size

    @property
    def ggtt_size(self) -> typing.Optional[int]:
        if self._ggtt_size is None:
            self.helper_get_debugfs_selfconfig()

        return self._ggtt_size

    @property
    def contexts(self) -> typing.Optional[int]:
        if self._contexts is None:
            self.helper_get_debugfs_selfconfig()

        return self._contexts

    @property
    def doorbells(self) -> typing.Optional[int]:
        if self._doorbells is None:
            self.helper_get_debugfs_selfconfig()

        return self._doorbells

    @property
    def tile_mask(self) -> typing.Optional[int]:
        if self._tile_mask is None:
            self.helper_get_debugfs_selfconfig()

        return self._tile_mask

    @property
    def gt_nums(self) -> typing.List[int]:
        self._gt_nums = self.get_gt_num_from_sysfs()
        if not self._gt_nums:
            logger.warning("VM sysfs: missing GT index")
            self._gt_nums = [0]

        return self._gt_nums

    def get_gt_num_from_sysfs(self) -> typing.List[int]:
        # Get GT number of VF passed to a VM, based on an exisitng a sysfs path
        vm_gt_num = []
        if self.dir_exists(posixpath.join(self.sysfs_prefix_path, 'gt/gt0')):
            vm_gt_num.append(0)
        if self.dir_exists(posixpath.join(self.sysfs_prefix_path, 'gt/gt1')):
            vm_gt_num.append(1)

        return vm_gt_num

    def get_drm_driver_name(self) -> str:
        return self.drm_driver_name

    def get_igt_config(self) -> VmtbIgtConfig:
        return self.igt_config

    @Decorators.timeout_signal
    def poweron(self) -> None:
        logger.debug('Powering on VM%s', self.vmnum)
        if self.is_running():
            logger.warning('VM%s already running', self.vmnum)
            return

        command = self.__get_popen_command()
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
            self.ga = GuestAgentBackend(self.questagent_sockpath, 300)
            self.qm = QmpMonitor(self.qmp_sockpath, 300)
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
                os.remove(self.questagent_sockpath)
                os.remove(self.qmp_sockpath)
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
            logger.debug('Running %s on VM%s with pid %s', command, self.vmnum, pid)
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
        # TODO: implement, currently no-op to fulfill MachineInterface requirement
        logger.warning("VirtualMachine.dir_list() is not implemented yet!")
        return []

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

    # helper_convert_units_to_bytes - convert size with units to bytes
    # @size_str: multiple-byte unit size with suffix (K/M/G)
    # Returns: size in bytes
    # TODO: function perhaps could be moved to some new utils module
    # improve - consider regex to handle various formats eg. both M and MB
    def helper_convert_units_to_bytes(self, size_str: str) -> int:
        size_str = size_str.upper()
        size_int = 0

        if size_str.endswith('B'):
            size_int = int(size_str[0:-1])
        elif size_str.endswith('K'):
            size_int = int(size_str[0:-1]) * 1024
        elif size_str.endswith('M'):
            size_int = int(size_str[0:-1]) * 1024**2
        elif size_str.endswith('G'):
            size_int = int(size_str[0:-1]) * 1024**3

        return size_int

    # helper_get_debugfs_selfconfig - read resources allocated to VF from debugfs:
    # /sys/kernel/debug/dri/@card/gt@gt_num/iov/self_config
    # @card: card number
    # @gt_num: GT instance number
    def helper_get_debugfs_selfconfig(self, card: int = 0, gt_num: int = 0) -> None:
        path = posixpath.join(f'/sys/kernel/debug/dri/{card}/gt{gt_num}/iov/self_config')
        out = self.read_file_content(path)

        for line in out.splitlines():
            param, value = line.split(':')

            if param == 'GGTT size':
                self._ggtt_size = self.helper_convert_units_to_bytes(value)
            elif param == 'LMEM size':
                self._lmem_size = self.helper_convert_units_to_bytes(value)
            elif param == 'contexts':
                self._contexts = int(value)
            elif param == 'doorbells':
                self._doorbells = int(value)
            elif param == 'tile mask':
                self._tile_mask = int(value, base=16)
