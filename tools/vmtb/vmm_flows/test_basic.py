# SPDX-License-Identifier: MIT
# Copyright © 2024-2026 Intel Corporation

import logging
import time
from typing import List, Tuple

import pytest

from bench.configurators.vgpu_profile_config import VfProvisioningMode, VfSchedulingMode
from bench.executors.gem_wsim import (ONE_CYCLE_DURATION_MS,
                                      PREEMPT_10MS_WORKLOAD, GemWsim,
                                      GemWsimResult,
                                      gem_wsim_parallel_exec_and_check)
from bench.executors.igt import IgtExecutor, IgtType
from bench.helpers.helpers import (driver_check, igt_check, igt_run_check,
                                   modprobe_driver_run_check)
from vmm_flows.conftest import (VmmTestingConfig, VmmTestingSetup,
                                idfn_test_config)

logger = logging.getLogger(__name__)

WL_ITERATIONS_10S = 1000
WL_ITERATIONS_30S = 3000
MS_IN_SEC = 1000
DELAY_FOR_WORKLOAD_SEC = 2 # Waiting gem_wsim to be running [seconds]
DELAY_FOR_RELOAD_SEC = 3 # Waiting before driver reloading [seconds]


def set_test_config(test_variants: List[Tuple[int, VfProvisioningMode, VfSchedulingMode]],
                    max_vms: int = 2, vf_driver_load: bool = True) -> List[VmmTestingConfig]:
    """Helper function to provide a parametrized test with a list of test configuration variants."""
    logger.debug("Init test variants: %s", test_variants)
    test_configs: List[VmmTestingConfig] = []

    for config in test_variants:
        (num_vfs, provisioning_mode, scheduling_mode) = config
        test_configs.append(VmmTestingConfig(num_vfs, max_vms, provisioning_mode, scheduling_mode,
                                             auto_probe_vm_driver=vf_driver_load))

    return test_configs


test_variants_1 = [(1, VfProvisioningMode.VGPU_PROFILE, VfSchedulingMode.DEFAULT_PROFILE),
                   (2, VfProvisioningMode.VGPU_PROFILE, VfSchedulingMode.DEFAULT_PROFILE),
                   (2, VfProvisioningMode.AUTO, VfSchedulingMode.INFINITE)]

@pytest.mark.parametrize('setup_vms', set_test_config(test_variants_1), ids=idfn_test_config, indirect=['setup_vms'])
class TestVmSetup:
    """Verify basic virtualization setup:
    - probe PF and VFIO drivers (host)
    - enable and provision VFs (automatic or manual with vGPU profile)
    - power on VMs with assigned VFs
    - probe VF driver (guest)
    - shutdown VMs, reset provisioning and disable VFs
    """
    def test_vm_boot(self, setup_vms):
        logger.info("Test VM boot: power on VM and probe VF driver")
        ts: VmmTestingSetup = setup_vms

        for vm in ts.vms:
            logger.info("[%s] Verify VF DRM driver is loaded in a guest OS", vm)
            assert driver_check(vm)


test_variants_2 = [(1, VfProvisioningMode.VGPU_PROFILE, VfSchedulingMode.DEFAULT_PROFILE),
                   (2, VfProvisioningMode.VGPU_PROFILE, VfSchedulingMode.DEFAULT_PROFILE),
                   (4, VfProvisioningMode.VGPU_PROFILE, VfSchedulingMode.DEFAULT_PROFILE)]

@pytest.mark.parametrize('setup_vms', set_test_config(test_variants_2), ids=idfn_test_config, indirect=['setup_vms'])
class TestVmWorkload:
    """Verify basic IGT workload execution a VM(s):
    - exec_store: basic store submissions on single/multiple VMs
    - gem_wsim: workload simulator running in parallel on multiple VMs
    """
    def test_store(self, setup_vms):
        logger.info("Test VM execution: exec_store")
        ts: VmmTestingSetup = setup_vms
        igt_worklads: List[IgtExecutor] = []

        for vm in ts.vms:
            logger.info("[%s] Execute basic WL", vm)
            igt_worklads.append(IgtExecutor(vm, IgtType.EXEC_STORE))

        for igt in igt_worklads:
            logger.info("[%s] Verify result of basic WL", igt.target)
            assert igt_check(igt)

        logger.info("[Host %s] Verify result of basic WL", ts.get_dut())
        igt_run_check(ts.host, IgtType.EXEC_STORE)

    def test_wsim(self, setup_vms):
        logger.info("Test VM execution: gem_wsim")
        ts: VmmTestingSetup = setup_vms

        if ts.get_num_vms() < 2:
            pytest.skip("Test scenario not supported for 1xVM setup ")

        # Single workload takes 10ms GPU time, multiplied by 1000 iterations
        # gives the expected 10s duration and 100 workloads/sec
        expected = GemWsimResult(ONE_CYCLE_DURATION_MS * WL_ITERATIONS_10S * len(ts.vms) / MS_IN_SEC,
                                 MS_IN_SEC/ONE_CYCLE_DURATION_MS / len(ts.vms))

        # Check preemptible workload
        result = gem_wsim_parallel_exec_and_check(ts.vms, PREEMPT_10MS_WORKLOAD, WL_ITERATIONS_10S, expected)
        logger.info("Execute wsim parallel on VMs - results: %s", result)


test_variants_3 = [(2, VfProvisioningMode.VGPU_PROFILE, VfSchedulingMode.DEFAULT_PROFILE),
                   (4, VfProvisioningMode.VGPU_PROFILE, VfSchedulingMode.DEFAULT_PROFILE)]

@pytest.mark.parametrize('setup_vms', set_test_config(test_variants=test_variants_3, max_vms=4, vf_driver_load=False),
                         ids = idfn_test_config, indirect=['setup_vms'])
class TestVfDriverLoadRemove:
    """Verify VF (guest) driver load or remove doesn't affect execution on the other VM:
    - probe VF driver on the last VM while the first VM is running workload
    - remove VF driver on the first VM while the last VM is running workload
    - reload previously removed VF driver on the same VM
    """
    def test_load(self, setup_vms):
        logger.info("Test VM driver load: VF driver probe while other VM executes workload")
        ts: VmmTestingSetup = setup_vms

        vm_first = ts.vms[0]
        vm_last = ts.vms[-1]

        logger.info("[%s] Load VF driver and run basic WL - first VM", vm_first)
        assert modprobe_driver_run_check(vm_first)

        expected_elapsed_sec = ONE_CYCLE_DURATION_MS * WL_ITERATIONS_30S / MS_IN_SEC
        gem_wsim = GemWsim(vm_first, 1, WL_ITERATIONS_30S, PREEMPT_10MS_WORKLOAD)
        time.sleep(DELAY_FOR_WORKLOAD_SEC)
        assert gem_wsim.is_running()

        logger.info("[%s] Load VF driver - last VM", vm_last)
        assert modprobe_driver_run_check(vm_last)

        result = gem_wsim.wait_results()
        assert expected_elapsed_sec * 0.8 < result.elapsed_sec < expected_elapsed_sec * 1.2

    def test_reload(self, setup_vms):
        logger.info("Test VM driver reload: VF driver remove is followed by probe while other VM executes workload")
        ts: VmmTestingSetup = setup_vms

        vm_first = ts.vms[0]
        vm_last = ts.vms[-1]

        logger.info("[%s] Run basic WL - last VM", vm_last)
        expected_elapsed_sec = ONE_CYCLE_DURATION_MS * WL_ITERATIONS_30S / MS_IN_SEC
        gem_wsim = GemWsim(vm_last, 1, WL_ITERATIONS_30S, PREEMPT_10MS_WORKLOAD)
        time.sleep(DELAY_FOR_WORKLOAD_SEC)
        assert gem_wsim.is_running()

        logger.info("[%s] Remove VF driver - first VM", vm_first)
        rmmod_pid = vm_first.execute(f'modprobe -rf {vm_first.get_drm_driver_name()}')
        assert vm_first.execute_wait(rmmod_pid).exit_code == 0

        time.sleep(DELAY_FOR_RELOAD_SEC)

        logger.info("[%s] Reload VF driver and run basic WL - first VM", vm_first)
        assert modprobe_driver_run_check(vm_first)
        assert igt_run_check(vm_first, IgtType.EXEC_STORE)

        result = gem_wsim.wait_results()
        assert expected_elapsed_sec * 0.8 < result.elapsed_sec < expected_elapsed_sec * 1.2
