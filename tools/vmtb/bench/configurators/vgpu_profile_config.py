# SPDX-License-Identifier: MIT
# Copyright © 2024-2026 Intel Corporation

import logging
from enum import Enum
from pathlib import Path

from bench import exceptions
from bench.configurators.pci import GpuModel, get_vgpu_profiles_file
from bench.configurators.vgpu_profile import (VgpuProfile,
                                              VgpuProfilesDefinitions,
                                              VgpuProfilesJsonReader,
                                              VgpuResourcesConfig,
                                              VgpuSchedulerConfig,
                                              VgpuSecurityConfig)

logger = logging.getLogger('DeviceConfigurator')


class VfProvisioningMode(Enum):
    VGPU_PROFILE = 0
    AUTO = 1


class VfSchedulingMode(str, Enum):
    INFINITE = 'Infinite' # Infinite EQ/PT - HW default
    DEFAULT_PROFILE = 'Default_Profile' # Default vGPU scheduler profile
    FLEXIBLE_30FPS = 'Flexible_30fps_GPUTimeSlicing'
    FIXED_30FPS = 'Fixed_30fps_GPUTimeSlicing'
    FLEXIBLE_BURSTABLE_QOS = 'Flexible_BurstableQoS_GPUTimeSlicing'

    def __str__(self) -> str:
        return str.__str__(self)


class VgpuProfileConfigurator:
    def __init__(self, vgpu_profiles_dir: Path, gpu_model: GpuModel = GpuModel.Unknown) -> None:
        self.gpu_model: GpuModel = gpu_model
        self.vgpu_profiles_dir: Path = vgpu_profiles_dir
        self.supported_vgpu_profiles: VgpuProfilesDefinitions = self.query_vgpu_profiles()

    def __helper_create_vgpu_json_path(self, vgpu_resource_dir: Path) -> Path:
        vgpu_device_file = get_vgpu_profiles_file(self.gpu_model)
        vgpu_json_file_path = vgpu_resource_dir / vgpu_device_file

        if not vgpu_json_file_path.exists():
            logger.error("vGPU profiles JSON file not found in %s", vgpu_resource_dir)
            raise exceptions.VgpuProfileError(f'vGPU profiles JSON file not found in {vgpu_resource_dir}')

        return vgpu_json_file_path

    def query_vgpu_profiles(self) -> VgpuProfilesDefinitions:
        """Get all vGPU profiles supported for a given GPU device."""
        json_reader = VgpuProfilesJsonReader(self.__helper_create_vgpu_json_path(self.vgpu_profiles_dir))
        return json_reader.vgpu_profiles

    def select_vgpu_resources_profile(self, requested_num_vfs: int) -> VgpuResourcesConfig:
        """Find vGPU profile matching requested number of VFs.
        In case exact match cannot be found, try to fit similar profile with up to 2 more VFs, for example:
        - if requested profile with 3 VFs is not available, return close config with 4 VFs.
        - if requested profile with neither 9 VFs, nor with 10 or 11 VFs is available - throw 'not found' exeception.
        """
        vgpu_resources_config = VgpuResourcesConfig()

        for pf_resource in self.supported_vgpu_profiles.pf_resources:
            if pf_resource.profile_name == self.supported_vgpu_profiles.pf_resource_default:
                vgpu_resources_config.pfLmem = pf_resource.local_memory_ecc_on
                vgpu_resources_config.pfContexts = pf_resource.contexts
                vgpu_resources_config.pfDoorbells = pf_resource.doorbells
                vgpu_resources_config.pfGgtt = pf_resource.ggtt_size

        is_vf_resource_found = False
        for vf_resource in self.supported_vgpu_profiles.vf_resources:
            current_num_vfs = vf_resource.vf_count

            if current_num_vfs == requested_num_vfs:
                is_vf_resource_found = True # Exact match
            elif requested_num_vfs < current_num_vfs <= requested_num_vfs + 2:
                logger.debug("Unable to find accurate vGPU profile but have similar: %s", vf_resource.profile_name)
                is_vf_resource_found = True # Approximate match

            if is_vf_resource_found:
                vgpu_resources_config.vfLmem = vf_resource.local_memory_ecc_on
                vgpu_resources_config.vfContexts = vf_resource.contexts
                vgpu_resources_config.vfDoorbells = vf_resource.doorbells
                vgpu_resources_config.vfGgtt = vf_resource.ggtt_size
                break

        if not is_vf_resource_found:
            logger.error("vGPU VF resources profile %sxVF not found!", requested_num_vfs)
            raise exceptions.VgpuProfileError(f'vGPU VF resources profile {requested_num_vfs}xVF not found!')

        return vgpu_resources_config

    def select_vgpu_scheduler_profile(self, requested_num_vfs: int,
                                      requested_scheduler: VfSchedulingMode) -> VgpuSchedulerConfig:
        # Function eval is needed to calculate VF EQ/PT for num_vfs
        # Disable eval warning
        # pylint: disable=W0123
        vgpu_scheduler_config = VgpuSchedulerConfig()

        if requested_scheduler is VfSchedulingMode.INFINITE:
            return vgpu_scheduler_config

        for scheduler in self.supported_vgpu_profiles.scheduler_configs:
            if scheduler.profile_name == requested_scheduler:
                vgpu_scheduler_config.scheduleIfIdle = scheduler.schedule_if_idle
                vgpu_scheduler_config.pfExecutionQuanta = scheduler.pf_execution_quanta
                vgpu_scheduler_config.pfPreemptionTimeout = scheduler.pf_preemption_timeout

                lambda_vf_eq = eval(scheduler.vf_execution_quanta)
                lambda_vf_eq_result = lambda_vf_eq(requested_num_vfs)

                lambda_vf_pt = eval(scheduler.vf_preemption_timeout)
                lambda_vf_pt_result = lambda_vf_pt(requested_num_vfs)

                vgpu_scheduler_config.vfExecutionQuanta = lambda_vf_eq_result
                vgpu_scheduler_config.vfPreemptionTimeout = lambda_vf_pt_result

        return vgpu_scheduler_config

    def select_vgpu_security_profile(self) -> VgpuSecurityConfig:
        # Currently supports only default security profile
        vgpu_security_config = VgpuSecurityConfig()

        for security_profile in self.supported_vgpu_profiles.security_configs:
            if security_profile.profile_name == self.supported_vgpu_profiles.security_config_default:
                vgpu_security_config.reset_after_vf_switch = security_profile.reset_after_vf_switch
                vgpu_security_config.guc_sampling_period = security_profile.guc_sampling_period
                vgpu_security_config.guc_threshold_cat_error = security_profile.guc_threshold_cat_error
                vgpu_security_config.guc_threshold_page_fault = security_profile.guc_threshold_page_fault
                vgpu_security_config.guc_threshold_h2g_storm = security_profile.guc_threshold_h2g_storm
                vgpu_security_config.guc_threshold_db_storm = security_profile.guc_threshold_db_storm
                vgpu_security_config.guc_treshold_gt_irq_storm = security_profile.guc_treshold_gt_irq_storm
                vgpu_security_config.guc_threshold_engine_reset = security_profile.guc_threshold_engine_reset

        return vgpu_security_config

    def get_vgpu_profile(self, requested_num_vfs: int, requested_scheduler: VfSchedulingMode) -> VgpuProfile:
        """Get vGPU profile for requested number of VFs, scheduler and security modes."""
        logger.info("Requested vGPU profile: %s VFs / scheduling: %s", requested_num_vfs, requested_scheduler)

        vgpu_profile: VgpuProfile = VgpuProfile()
        vgpu_profile.num_vfs = requested_num_vfs
        vgpu_profile.resources = self.select_vgpu_resources_profile(requested_num_vfs)

        if requested_scheduler is VfSchedulingMode.DEFAULT_PROFILE:
            requested_scheduler = VfSchedulingMode(self.supported_vgpu_profiles.scheduler_config_default)

        vgpu_profile.scheduler = self.select_vgpu_scheduler_profile(requested_num_vfs, requested_scheduler)
        vgpu_profile.security = self.select_vgpu_security_profile()

        return vgpu_profile
