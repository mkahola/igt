# SPDX-License-Identifier: MIT
# Copyright © 2024-2026 Intel Corporation

import logging
import re
import typing
from pathlib import Path

from bench import exceptions
from bench.executors.shell import ShellExecutor
from bench.machines.machine_interface import DEFAULT_TIMEOUT, MachineInterface

logger = logging.getLogger('GemWsim')


class GemWsimResult(typing.NamedTuple):
    elapsed_sec: float
    workloads_per_sec: float

# Basic workloads
ONE_CYCLE_DURATION_MS = 10
PREEMPT_10MS_WORKLOAD = (f'1.DEFAULT.{int(ONE_CYCLE_DURATION_MS * 1000 / 2)}.0.0'
                         f',2.DEFAULT.{int(ONE_CYCLE_DURATION_MS * 1000 / 2)}.-1.1')
NON_PREEMPT_10MS_WORKLOAD = f'X.1.0,X.2.0,{PREEMPT_10MS_WORKLOAD}'

class GemWsim(ShellExecutor):
    def __init__(self, target: MachineInterface,
                 num_clients: int = 1,
                 num_repeats: int = 1,
                 workload: str = PREEMPT_10MS_WORKLOAD,
                 timeout: int = DEFAULT_TIMEOUT) -> None:

        self.machine_id = str(target)
        wl_file_path = Path(workload)

        if wl_file_path.is_file():
            # Copy workload descriptor from the source host file
            # to the requested target machine (VM or host)
            workload = '/tmp/igt_workload.wsim'
            wl_desc = wl_file_path.read_text(encoding='utf-8')
            target.write_file_content(workload, wl_desc)

            logger.debug("IGT/wsim workload descriptor:\n%s", wl_desc)

        wsim_path = Path(target.get_igt_config().test_dir) / 'benchmarks' / 'gem_wsim'
        command = f'{wsim_path} -w {workload} -c {num_clients} -r {num_repeats}'

        super().__init__(target, command, timeout)

    def __str__(self) -> str:
        return f'gem_wsim({self.machine_id}:{self.pid})'

    def is_running(self) -> bool:
        return not self.status().exited

    def wait_results(self) -> GemWsimResult:
        proc_result = self.wait()
        if proc_result.exit_code == 0:
            logger.info('%s: %s', self, proc_result.stdout)
            # Try parse output ex.: 19.449s elapsed (102.836 workloads/s)
            pattern = r'(?P<elapsed>\d+(\.\d*)?|\.\d+)s elapsed \((?P<wps>\d+(\.\d*)?|\.\d+) workloads/s\)'
            match = re.search(pattern, proc_result.stdout, re.MULTILINE)
            if match:
                return GemWsimResult(float(match.group('elapsed')), float(match.group('wps')))
        raise exceptions.GemWsimError(f'{self}: exit_code: {proc_result.exit_code}'
                                      f' stdout: {proc_result.stdout} stderr: {proc_result.stderr}')

    def check_results(self) -> bool:
        """Verify gem_wsim execution results. Return True for workload success, False on fail."""
        # TODO: support also checking wsim workload: 'Verify Xe spinner batch completion'
        try:
            wl_results: GemWsimResult = self.wait_results()
        except exceptions.GemWsimError as exc:
            logger.error("[%s] WSIM failed: %s", self.target, exc)
            return False

        logger.debug("[%s] WSIM passed in %ss (%s WL/s)",
                      self.target, wl_results.elapsed_sec, wl_results.workloads_per_sec)
        return True


def gem_wsim_parallel_exec_and_check(vms: typing.List[MachineInterface], workload: str, iterations: int,
                                     expected: typing.Optional[GemWsimResult] = None) -> GemWsimResult:
    # launch on each VM in parallel
    wsim_procs = [GemWsim(vm, 1, iterations, workload) for vm in vms]
    for i, wsim in enumerate(wsim_procs):
        assert wsim.is_running(), f'GemWsim failed to start on VM{i}'

    results = [wsim.wait_results() for wsim in wsim_procs]
    if expected is not None:
        assert results[0].elapsed_sec > expected.elapsed_sec * 0.9
        assert results[0].workloads_per_sec > expected.workloads_per_sec * 0.9
    for r in results[1:]:
        # check wps ratio ~1.0 with 10% tolerance
        assert 0.9 < r.workloads_per_sec / results[0].workloads_per_sec < 1.1
        # check elapsed ratio ~1.0 with 10% tolerance
        assert 0.9 < r.elapsed_sec / results[0].elapsed_sec < 1.1
    # return first result, all other are asserted to be ~same
    return results[0]
