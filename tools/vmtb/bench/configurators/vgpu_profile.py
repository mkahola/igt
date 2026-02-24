# SPDX-License-Identifier: MIT
# Copyright © 2024-2026 Intel Corporation

import json
import logging
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List

from bench import exceptions

logger = logging.getLogger('VgpuProfile')


@dataclass
class VgpuResourcesConfig:
    pfLmem: int = 0
    pfContexts: int = 0
    pfDoorbells: int = 0
    pfGgtt: int = 0
    vfLmem: int = 0
    vfContexts: int = 0
    vfDoorbells: int = 0
    vfGgtt: int = 0


@dataclass
class VgpuSchedulerConfig:
    scheduleIfIdle: bool = False
    pfExecutionQuanta: int = 0
    pfPreemptionTimeout: int = 0
    vfExecutionQuanta: int = 0
    vfPreemptionTimeout: int = 0


@dataclass
class VgpuSecurityConfig:
    reset_after_vf_switch: bool = False
    guc_sampling_period: int = 0
    guc_threshold_cat_error: int = 0
    guc_threshold_page_fault: int = 0
    guc_threshold_h2g_storm: int = 0
    guc_threshold_db_storm: int = 0
    guc_treshold_gt_irq_storm: int = 0
    guc_threshold_engine_reset: int = 0


@dataclass
class VgpuProfile:
    num_vfs: int = 0
    scheduler: VgpuSchedulerConfig = field(default_factory=VgpuSchedulerConfig)
    resources: VgpuResourcesConfig = field(default_factory=VgpuResourcesConfig)
    security: VgpuSecurityConfig = field(default_factory=VgpuSecurityConfig)

    def print_parameters(self) -> None:
        logger.info(
            "\nvGPU Profile:\n"
            "   Num VFs = %s\n",
            self.num_vfs)
        self.print_resources_config()
        self.print_scheduler_config()
        logger.info(
            "\nSecurity:\n"
            "   Reset After Vf Switch = %s\n",
            self.security.reset_after_vf_switch
        )

    def print_resources_config(self) -> None:
        logger.info(
            "\nResources config:\n"
            "   PF:\n"
            "\tLMEM = %s B\n"
            "\tContexts = %s\n"
            "\tDoorbells = %s\n"
            "\tGGTT = %s B\n"
            "   VF:\n"
            "\tLMEM = %s B\n"
            "\tContexts = %s\n"
            "\tDoorbells = %s\n"
            "\tGGTT = %s B\n",
            self.resources.pfLmem, self.resources.pfContexts, self.resources.pfDoorbells, self.resources.pfGgtt,
            self.resources.vfLmem, self.resources.vfContexts, self.resources.vfDoorbells, self.resources.vfGgtt,
        )

    def print_scheduler_config(self) -> None:
        logger.info(
            "\nScheduling config:\n"
            "   Schedule If Idle = %s\n"
            "   PF:\n"
            "\tExecution Quanta = %s ms\n"
            "\tPreemption Timeout = %s us\n"
            "   VF:\n"
            "\tExecution Quanta = %s ms\n"
            "\tPreemption Timeout = %s us\n",
            self.scheduler.scheduleIfIdle,
            self.scheduler.pfExecutionQuanta, self.scheduler.pfPreemptionTimeout,
            self.scheduler.vfExecutionQuanta, self.scheduler.vfPreemptionTimeout
        )


# Structures for mapping vGPU profiles definition from JSON files
@dataclass
class VgpuProfilePfResourcesDefinition:
    profile_name: str
    local_memory_ecc_off: int
    local_memory_ecc_on: int
    contexts: int
    doorbells: int
    ggtt_size: int


@dataclass
class VgpuProfileVfResourcesDefinition:
    profile_name: str
    vf_count: int
    local_memory_ecc_off: int
    local_memory_ecc_on: int
    contexts: int
    doorbells: int
    ggtt_size: int


@dataclass
class VgpuProfileSchedulerDefinition:
    profile_name: str = 'N/A'
    schedule_if_idle: bool = False
    pf_execution_quanta: int = 0
    pf_preemption_timeout: int = 0
    vf_execution_quanta: str = ''   # To calculate based on number of VFs
    vf_preemption_timeout: str = '' # To calculate based on number of VFs


@dataclass
class VgpuProfileSecurityDefinition(VgpuSecurityConfig):
    profile_name: str = 'N/A'


@dataclass
class VgpuProfilesDefinitions:
    pf_resource_default: str
    pf_resources: List[VgpuProfilePfResourcesDefinition]
    vf_resource_default: str
    vf_resources: List[VgpuProfileVfResourcesDefinition]
    scheduler_config_default: str
    scheduler_configs: List[VgpuProfileSchedulerDefinition]
    security_config_default: str
    security_configs: List[VgpuProfileSecurityDefinition]


class VgpuProfilesJsonReader:
    def __init__(self, vgpu_json_path: Path) -> None:
        vgpu_profile_data = self.read_json_file(vgpu_json_path)
        self.vgpu_profiles: VgpuProfilesDefinitions = self.parse_json_file(vgpu_profile_data)

    def read_json_file(self, vgpu_json_file: Path) -> Any:
        if not Path(vgpu_json_file).exists():
            logger.error("vGPU profile JSON file not found: %s", vgpu_json_file)
            raise exceptions.VgpuProfileError(f'vGPU profile JSON file not found: {vgpu_json_file}')

        with open(vgpu_json_file, mode='r', encoding='utf-8') as json_file:
            try:
                vgpu_json = json.load(json_file)
            except json.JSONDecodeError as exc:
                logger.error("Invalid vGPU profile JSON format: %s", exc)
                raise exceptions.VgpuProfileError('Invalid vGPU profile definition JSON format')

        return vgpu_json

    def __parse_pf_resource_profiles(self, pf_profiles: Dict) -> List[VgpuProfilePfResourcesDefinition]:
        pf_resources: List[VgpuProfilePfResourcesDefinition] = []

        for pf_profile_name in pf_profiles.keys():
            lmem_ecc_off = pf_profiles[pf_profile_name]['LocalMemoryEccOff']
            lmem_ecc_on = pf_profiles[pf_profile_name]['LocalMemoryEccOn']
            contexts = pf_profiles[pf_profile_name]['Contexts']
            doorbells = pf_profiles[pf_profile_name]['Doorbells']
            ggtt_size = pf_profiles[pf_profile_name]['GGTTSize']

            current_pf_resource = VgpuProfilePfResourcesDefinition(pf_profile_name,
                                                                   lmem_ecc_off,
                                                                   lmem_ecc_on,
                                                                   contexts,
                                                                   doorbells,
                                                                   ggtt_size)

            pf_resources.append(current_pf_resource)

        return pf_resources

    def __parse_vf_resource_profiles(self, vf_profiles: Dict) -> List[VgpuProfileVfResourcesDefinition]:
        vf_resources: List[VgpuProfileVfResourcesDefinition] = []

        for vf_profile_name in vf_profiles.keys():
            vf_count = vf_profiles[vf_profile_name]['VFCount']
            lmem_ecc_off = vf_profiles[vf_profile_name]['LocalMemoryEccOff']
            lmem_ecc_on = vf_profiles[vf_profile_name]['LocalMemoryEccOn']
            contexts = vf_profiles[vf_profile_name]['Contexts']
            doorbells = vf_profiles[vf_profile_name]['Doorbells']
            ggtt_size = vf_profiles[vf_profile_name]['GGTTSize']

            current_vf_resource = VgpuProfileVfResourcesDefinition(vf_profile_name,
                                                                   vf_count,
                                                                   lmem_ecc_off,
                                                                   lmem_ecc_on,
                                                                   contexts,
                                                                   doorbells,
                                                                   ggtt_size)

            vf_resources.append(current_vf_resource)

        return vf_resources

    def __parse_scheduler_profiles(self, scheduler_profiles: Dict) -> List[VgpuProfileSchedulerDefinition]:
        scheduler_configs: List[VgpuProfileSchedulerDefinition] = []

        for scheduler_profile_name in scheduler_profiles.keys():
            schedule_if_idle = scheduler_profiles[scheduler_profile_name]['GPUTimeSlicing']['ScheduleIfIdle']
            pf_eq = scheduler_profiles[scheduler_profile_name]['GPUTimeSlicing']['PFExecutionQuantum']
            pf_pt = scheduler_profiles[scheduler_profile_name]['GPUTimeSlicing']['PFPreemptionTimeout']
            vf_eq = scheduler_profiles[scheduler_profile_name]['GPUTimeSlicing']['VFAttributes']['VFExecutionQuantum']
            vf_pt = scheduler_profiles[scheduler_profile_name]['GPUTimeSlicing']['VFAttributes']['VFPreemptionTimeout']

            current_scheduler = VgpuProfileSchedulerDefinition(scheduler_profile_name,
                                                               schedule_if_idle,
                                                               pf_eq, pf_pt,
                                                               vf_eq, vf_pt)

            scheduler_configs.append(current_scheduler)

        return scheduler_configs

    def __parse_security_profiles(self, security_profiles: Dict) -> List[VgpuProfileSecurityDefinition]:
        security_configs: List[VgpuProfileSecurityDefinition] = []

        for security_profile_name in security_profiles.keys():
            reset_after_vf_switch = security_profiles[security_profile_name]['ResetAfterVfSwitch']
            guc_sampling_period = security_profiles[security_profile_name]['GuCSamplingPeriod']
            guc_threshold_cat_error = security_profiles[security_profile_name]['GuCThresholdCATError']
            guc_threshold_page_fault = security_profiles[security_profile_name]['GuCThresholdPageFault']
            guc_threshold_h2g_storm = security_profiles[security_profile_name]['GuCThresholdH2GStorm']
            guc_threshold_db_storm = security_profiles[security_profile_name]['GuCThresholdDbStorm']
            guc_treshold_gt_irq_storm = security_profiles[security_profile_name]['GuCThresholdGTIrqStorm']
            guc_threshold_engine_reset = security_profiles[security_profile_name]['GuCThresholdEngineReset']

            # VgpuSecurityConfig (base class) params go first, therefore profile name
            # is the last param on the VgpuProfileSecurityDefinition initialization list in this case
            current_security_config = VgpuProfileSecurityDefinition(reset_after_vf_switch,
                                                                    guc_sampling_period,
                                                                    guc_threshold_cat_error,
                                                                    guc_threshold_page_fault,
                                                                    guc_threshold_h2g_storm,
                                                                    guc_threshold_db_storm,
                                                                    guc_treshold_gt_irq_storm,
                                                                    guc_threshold_engine_reset,
                                                                    security_profile_name)

            security_configs.append(current_security_config)

        return security_configs

    def parse_json_file(self, vgpu_json: Dict) -> VgpuProfilesDefinitions:
        pf_resource_default =  vgpu_json['PFResources']['Default']
        pf_resources = self.__parse_pf_resource_profiles(vgpu_json['PFResources']['Profile'])

        vf_resource_default =  vgpu_json['vGPUResources']['Default']
        vf_resources = self.__parse_vf_resource_profiles(vgpu_json['vGPUResources']['Profile'])

        scheduler_default =  vgpu_json['vGPUScheduler']['Default']
        scheduler_configs = self.__parse_scheduler_profiles(vgpu_json['vGPUScheduler']['Profile'])

        security_default =  vgpu_json['vGPUSecurity']['Default']
        security_configs = self.__parse_security_profiles(vgpu_json['vGPUSecurity']['Profile'])

        return VgpuProfilesDefinitions(pf_resource_default, pf_resources, vf_resource_default, vf_resources,
                                       scheduler_default, scheduler_configs, security_default, security_configs)
