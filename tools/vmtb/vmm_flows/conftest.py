# SPDX-License-Identifier: MIT
# Copyright © 2024-2026 Intel Corporation

import json
import logging
import re
import typing
from dataclasses import dataclass
from pathlib import Path

import pytest

from bench import exceptions
from bench.configurators.vgpu_profile import VgpuProfile
from bench.configurators.vgpu_profile_config import (VfProvisioningMode,
                                                     VfSchedulingMode,
                                                     VgpuProfileConfigurator)
from bench.configurators.vmtb_config import VmtbConfigurator
from bench.helpers.log import HOST_DMESG_FILE
from bench.machines.host import Device, Host
from bench.machines.virtual.vm import VirtualMachine

logger = logging.getLogger('Conftest')


def pytest_addoption(parser):
    parser.addoption('--vm-image',
                     action='store',
                     help='OS image to boot on VM')
    parser.addoption('--card',
                     action='store',
                     help='Device card index for test execution')


# Label indicating Max VFs configurarion variant, intended for pass to VmmTestingConfig.
MAX_VFS = "Max"


@dataclass
class VmmTestingConfig:
    """Structure represents test configuration used by a setup fixture.

    Available settings:
    - num_vfs: requested number of VFs to enable
    - max_num_vms: maximal number of VMs (the value can be different than enabled number of VFs)
    - provisioning_mode: auto (fair resources allocation by a driver) or vGPU profile
    - scheduling_mode: requested vGPU scheduling profile (infinite maps to default 0's)
    - auto_poweron_vm: assign VFs and power on VMs automatically in setup fixture
    - auto_probe_vm_driver: probe guest DRM driver in setup fixture (VM must be powered on)
    - unload_host_drivers_on_teardown: unload host DRM drivers in teardown fixture
    - wa_reduce_vf_lmem: workaround to reduce VF LMEM (for save-restore/migration tests speed-up)
    """
    num_vfs: int = 1
    max_num_vms: int = 2
    provisioning_mode: VfProvisioningMode = VfProvisioningMode.AUTO
    scheduling_mode: VfSchedulingMode = VfSchedulingMode.INFINITE

    auto_poweron_vm: bool = True
    auto_probe_vm_driver: bool = True
    unload_host_drivers_on_teardown: bool = False
    enable_max_vfs: bool = False
    # Temporary W/A: reduce size of LMEM assigned to VFs to speed up a VF state save-restore process
    wa_reduce_vf_lmem: bool = False

    def __post_init__(self):
        if self.num_vfs is MAX_VFS:
            self.enable_max_vfs = True
            self.num_vfs = 0 # Actual value set in VmmTestingSetup

    def __str__(self) -> str:
        test_config_id = (f'{self.num_vfs if not self.enable_max_vfs else "Max"}VF'
                          + f'-(P:{self.provisioning_mode.name} S:{self.scheduling_mode.name})')
        return test_config_id

    def __repr__(self) -> str:
        return (f'\nVmmTestingConfig:'
                f'\nNum VFs = {self.num_vfs if not self.enable_max_vfs else "Max"} / max num VMs = {self.max_num_vms}'
                f'\nVF provisioning mode = {self.provisioning_mode.name}'
                f'\nVF scheduling mode = {self.scheduling_mode.name}'
                f'\nSetup flags:'
                f'\n\tVM - auto power-on = {self.auto_poweron_vm}'
                f'\n\tVM - auto DRM driver probe = {self.auto_probe_vm_driver}'
                f'\n\tHost - unload drivers on teardown = {self.unload_host_drivers_on_teardown}'
                f'\n\tW/A - reduce VF LMEM (improves migration time) = {self.wa_reduce_vf_lmem}')


class VmmTestingSetup:
    def __init__(self, vmtb_config: VmtbConfigurator, cmdline_config, host, testing_config):
        self.testing_config: VmmTestingConfig = testing_config
        self.host: Host = host

        self.dut_index = vmtb_config.get_host_config().card_index if cmdline_config['card_index'] is None \
                         else int(cmdline_config['card_index'])
        self.guest_os_image = vmtb_config.get_guest_config().os_image_path if cmdline_config['vm_image'] is None \
                         else cmdline_config['vm_image']

        self.host.drm_driver_name = vmtb_config.get_host_config().driver
        self.host.igt_config = vmtb_config.get_host_config().igt_config

        self.host.load_drivers()
        self.host.discover_devices()
        self.dut: Device = self.host.get_device(self.dut_index)
        self.total_vfs: int = self.get_dut().driver.get_totalvfs()

        # VF migration requires vendor specific VFIO driver (e.g. xe-vfio-pci)
        vf_migration_support: bool = self.host.is_driver_loaded(f'{self.host.drm_driver_name}-vfio-pci')

        logger.info("\nDUT info:"
                    "\n\tCard index: %s"
                    "\n\tPCI BDF: %s "
                    "\n\tDevice ID: %s (%s)"
                    "\n\tHost DRM driver: %s"
                    "\n\tMax VFs supported: %s"
                    "\n\tVF migration supported: %s",
                    self.dut_index,
                    self.get_dut().pci_info.bdf,
                    self.get_dut().pci_info.devid, self.get_dut().gpu_model,
                    self.get_dut().driver.get_name(),
                    self.total_vfs,
                    vf_migration_support)

        vmtb_root_path = vmtb_config.vmtb_config_file.parent
        self.vgpu_profiles_dir = vmtb_root_path / vmtb_config.config.vgpu_profiles_path
        # Device specific wsim descriptors directory path, e.g.:
        # [vmtb_root]/vmm_flows/resources/wsim/ptl (last subdir is lowercase key/name from pci.GpuModel class)
        self.wsim_wl_dir = vmtb_root_path / vmtb_config.config.wsim_wl_path / self.get_dut().gpu_model.name.lower()

        if (self.testing_config.provisioning_mode is VfProvisioningMode.VGPU_PROFILE
            or self.testing_config.scheduling_mode is not VfSchedulingMode.INFINITE):
            self.vgpu_profile: VgpuProfile = self.get_vgpu_profile()

        if self.testing_config.provisioning_mode is VfProvisioningMode.AUTO and self.testing_config.enable_max_vfs:
            self.testing_config.num_vfs = self.total_vfs

        # Start maximum requested number of VMs, but not more than VFs supported by the given vGPU profile
        self.vms: typing.List[VirtualMachine] = [
            VirtualMachine(vm_idx, self.guest_os_image,
                           vmtb_config.get_guest_config().driver,
                           vmtb_config.get_guest_config().igt_config,
                           vf_migration_support)
            for vm_idx in range(min(self.testing_config.num_vfs, self.testing_config.max_num_vms))]

    def get_vgpu_profile(self) -> VgpuProfile:
        configurator = VgpuProfileConfigurator(self.vgpu_profiles_dir, self.get_dut().gpu_model)
        if self.testing_config.enable_max_vfs:
            # Get a vGPU profile with the most VFs (not necessarily equal to sysfs/sriov_totalvfs)
            self.testing_config.num_vfs = max(configurator.supported_vgpu_profiles.vf_resources,
                                              key=lambda profile: profile.vf_count).vf_count
            logger.debug("Max VFs supported by vGPU profiles: %s", self.testing_config.num_vfs)

        try:
            vgpu_profile = configurator.get_vgpu_profile(self.testing_config.num_vfs,
                                                         self.testing_config.scheduling_mode)
        except exceptions.VgpuProfileError as exc:
            logger.error("Suitable vGPU profile not found: %s", exc)
            raise exceptions.VgpuProfileError('Invalid test setup - vGPU profile not found!')

        return vgpu_profile

    def get_dut(self) -> Device:
        if self.dut is None:
            logger.error("Invalid VMTB config - DRM card%s is not bound to %s driver",
                         self.dut_index, self.host.drm_driver_name)
            raise exceptions.VmtbConfigError(f'Invalid VMTB config - DRM card{self.dut_index} device not supported')

        return self.dut

    @property
    def get_vm(self):
        return self.vms

    def get_num_vms(self) -> int:
        return len(self.vms)

    def poweron_vms(self):
        for vm in self.vms:
            vm.poweron()

    def poweroff_vms(self):
        for vm in self.vms:
            if vm.is_running():
                try:
                    vm.poweroff()
                except Exception as exc:
                    self.testing_config.unload_host_drivers_on_teardown = True
                    logger.warning("Error on VM%s poweroff (%s)", vm.vmnum, exc)

        if self.testing_config.unload_host_drivers_on_teardown:
            raise exceptions.GuestError('VM poweroff issue - cleanup on test teardown')

    def teardown(self):
        try:
            self.poweroff_vms()
        except Exception as exc:
            logger.error("Error on test teardown (%s)", exc)
        finally:
            num_vfs = self.get_dut().get_current_vfs()
            self.get_dut().remove_vfs()
            self.get_dut().reset_provisioning(num_vfs)
            self.get_dut().cancel_work()

            if self.testing_config.unload_host_drivers_on_teardown:
                self.host.unload_drivers()


@pytest.fixture(scope='session', name='get_vmtb_config')
def fixture_get_vmtb_config(create_host_log, pytestconfig):
    VMTB_CONFIG_FILE = 'vmtb_config.json'
    # Pytest Config.rootpath points to the VMTB base directory
    vmtb_config_file_path: Path = pytestconfig.rootpath / VMTB_CONFIG_FILE
    return VmtbConfigurator(vmtb_config_file_path)


@pytest.fixture(scope='session', name='create_host_log')
def fixture_create_host_log():
    if HOST_DMESG_FILE.exists():
        HOST_DMESG_FILE.unlink()
    HOST_DMESG_FILE.touch()


@pytest.fixture(scope='session', name='get_cmdline_config')
def fixture_get_cmdline_config(request):
    cmdline_params = {}
    cmdline_params['vm_image'] = request.config.getoption('--vm-image')
    cmdline_params['card_index'] = request.config.getoption('--card')
    return cmdline_params


@pytest.fixture(scope='session', name='get_host')
def fixture_get_host():
    return Host()


@pytest.fixture(scope='class', name='setup_vms')
def fixture_setup_vms(get_vmtb_config, get_cmdline_config, get_host, request):
    """Arrange VM environment for the VMM Flows test execution.

    VM setup steps follow the configuration provided as VmmTestingConfig parameter, including:
    host drivers probe (DRM and VFIO), provision and enable VFs, boot VMs and load guest DRM driver.
    Tear-down phase covers test environment cleanup:
    shutdown VMs, reset provisioning, disable VMs and optional host drivers unload.

    The fixture is designed for test parametrization, as the input to the following test class decorator:
    @pytest.mark.parametrize('setup_vms', set_test_config(max_vms=N), ids=idfn_test_config, indirect=['setup_vms'])
    where 'set_test_config' provides request parameter with a VmmTestingConfig (usually list of configs).
    """
    tc: VmmTestingConfig = request.param
    logger.debug(repr(tc))

    host: Host = get_host
    ts: VmmTestingSetup = VmmTestingSetup(get_vmtb_config, get_cmdline_config, host, tc)

    device: Device = ts.get_dut()
    num_vfs = ts.testing_config.num_vfs
    num_vms = ts.get_num_vms()

    logger.info('[Test setup: %sVF-%sVM]', num_vfs, num_vms)

    if ts.testing_config.provisioning_mode is VfProvisioningMode.VGPU_PROFILE:
        # XXX: Double migration is slow on discrete GPUs (with VRAM),
        # As a workaround, reduce VRAM size assigned to VFs to speed up a save process.
        # This w/a should be removed when a save/restore time improves.
        if tc.wa_reduce_vf_lmem and device.has_lmem():
            logger.debug("W/A: reduce VFs LMEM quota to accelerate state save")
            org_vgpu_profile_vfLmem = ts.vgpu_profile.resources.vfLmem
            # Assign max 512 MB to VF
            ts.vgpu_profile.resources.vfLmem = min(ts.vgpu_profile.resources.vfLmem // 2, 536870912)

        ts.vgpu_profile.print_parameters()
        device.provision(ts.vgpu_profile)

        # XXX: cleanup counterpart for VFs LMEM quota workaround:
        # restore original value after vGPU profile applied
        if tc.wa_reduce_vf_lmem and device.has_lmem():
            ts.vgpu_profile.resources.vfLmem = org_vgpu_profile_vfLmem

    else:
        device.driver.restore_auto_provisioning()

    assert device.create_vf(num_vfs) == num_vfs

    if (ts.testing_config.provisioning_mode is VfProvisioningMode.AUTO and
        ts.testing_config.scheduling_mode is not VfSchedulingMode.INFINITE):
        # Auto resources provisioning with concrete scheduling (i.e. different than HW default: infinite).
        # Scheduler params override must be done after enabling VFs
        # to allow hard resources auto provisioning by KMD.
        ts.vgpu_profile.print_scheduler_config()
        device.provision_scheduling(num_vfs, ts.vgpu_profile)

    if tc.auto_poweron_vm:
        bdf_list = [device.get_vf_bdf(vf) for vf in range(1, num_vms + 1)]
        for vm, bdf in zip(ts.get_vm, bdf_list):
            vm.assign_vf(bdf)

        ts.poweron_vms()

        if tc.auto_probe_vm_driver:
            for vm in ts.get_vm:
                vm.load_drm_driver()
                vm.discover_devices()

    logger.info('[Test execution: %sVF-%sVM]', num_vfs, num_vms)
    yield ts

    logger.info('[Test teardown: %sVF-%sVM]', num_vfs, num_vms)
    ts.teardown()


# Obsolete fixtures 'create_Xhost_Yvm' - 'fixture_setup_vms' is preferred
@pytest.fixture(scope='function')
def create_1host_1vm(get_vmtb_config, get_cmdline_config, get_host):
    num_vfs, num_vms = 1, 1
    ts: VmmTestingSetup = VmmTestingSetup(get_vmtb_config, get_cmdline_config, get_host,
                                          VmmTestingConfig(num_vfs, num_vms))

    logger.info('[Test setup: %sVF-%sVM]', num_vfs, num_vms)
    logger.debug(repr(ts.testing_config))

    logger.info('[Test execution: %sVF-%sVM]', num_vfs, num_vms)
    yield ts

    logger.info('[Test teardown: %sVF-%sVM]', num_vfs, num_vms)
    ts.teardown()


@pytest.fixture(scope='function')
def create_1host_2vm(get_vmtb_config, get_cmdline_config, get_host):
    num_vfs, num_vms = 2, 2
    ts: VmmTestingSetup = VmmTestingSetup(get_vmtb_config, get_cmdline_config, get_host,
                                          VmmTestingConfig(num_vfs, num_vms))

    logger.info('[Test setup: %sVF-%sVM]', num_vfs, num_vms)
    logger.debug(repr(ts.testing_config))

    logger.info('[Test execution: %sVF-%sVM]', num_vfs, num_vms)
    yield ts

    logger.info('[Test teardown: %sVF-%sVM]', num_vfs, num_vms)
    ts.teardown()


def idfn_test_config(test_config: VmmTestingConfig):
    """Provide test config ID in parametrized tests (e.g. test_something[V4].
    Usage: @pytest.mark.parametrize([...], ids=idfn_test_config, [...])
    """
    return str(test_config)


RESULTS_FILE = Path() / "results.json"
results = {
    "results_version": 10,
    "name": "results",
    "tests": {},
}


@pytest.hookimpl(hookwrapper=True)
def pytest_report_teststatus(report):
    yield
    with open(HOST_DMESG_FILE, 'r+', encoding='utf-8') as dmesg_file:
        dmesg = dmesg_file.read()
        test_string = re.findall('[A-Za-z_.]*::.*', report.nodeid)[0]
        results["name"] = f"vmtb_{test_string}"
        test_name = f"vmtb@{test_string}"
        if report.when == 'call':
            out = report.capstdout
            if report.passed:
                result = "pass"
                out = f"{test_name} passed"
            elif report.failed:
                result = "fail"
            else:
                result = "skip"
            result = {"out": out, "result": result, "time": {"start": 0, "end": report.duration},
                    "err": report.longreprtext, "dmesg": dmesg}
            results["tests"][test_name] = result
            dmesg_file.truncate(0)
        elif report.when == 'setup' and report.failed:
            result = {"out": report.capstdout, "result": "crash", "time": {"start": 0, "end": report.duration},
                    "err": report.longreprtext, "dmesg": dmesg}
            results["tests"][test_name] = result
            dmesg_file.truncate(0)


@pytest.hookimpl()
def pytest_sessionfinish():
    if RESULTS_FILE.exists():
        RESULTS_FILE.unlink()
    RESULTS_FILE.touch()
    jsonString = json.dumps(results, indent=2)
    with open(str(RESULTS_FILE), 'w',  encoding='utf-8') as f:
        f.write(jsonString)
