# SPDX-License-Identifier: MIT
# Copyright © 2024-2026 Intel Corporation

import json
import logging
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict

from bench import exceptions

logger = logging.getLogger('VmtbConfigurator')


@dataclass
class VmtbIgtConfig:
    test_dir: str
    tool_dir: str
    lib_dir: str
    result_dir: str
    options: str


@dataclass
class VmtbHostConfig:
    card_index: int
    driver: str
    igt_config: VmtbIgtConfig


@dataclass
class VmtbGuestConfig:
    os_image_path: str
    driver: str
    igt_config: VmtbIgtConfig


@dataclass
class VmtbConfig:
    host_config: VmtbHostConfig
    guest_config: VmtbGuestConfig
    vgpu_profiles_path: str
    wsim_wl_path: str
    guc_ver_path: str
    ci_host_dmesg_file: str


class VmtbConfigurator:
    def __init__(self, vmtb_config_file_path: Path) -> None:
        self.vmtb_config_file: Path = vmtb_config_file_path
        self.config: VmtbConfig = self.query_vmtb_config()

    def query_vmtb_config(self) -> VmtbConfig:
        json_reader = VmtbConfigJsonReader(self.vmtb_config_file)
        return json_reader.vmtb_config

    def get_host_config(self) -> VmtbHostConfig:
        return self.config.host_config

    def get_guest_config(self) -> VmtbGuestConfig:
        return self.config.guest_config


class VmtbConfigJsonReader:
    def __init__(self, config_json_path: Path) -> None:
        vgpu_profile_data = self.read_json_file(config_json_path)
        self.vmtb_config: VmtbConfig = self.parse_json_file(vgpu_profile_data)

    def read_json_file(self, config_json_file: Path) -> Any:
        if not config_json_file.exists():
            logger.error("VMTB config JSON file not found: %s", config_json_file)
            raise exceptions.VmtbConfigError(f'VMTB config JSON file not found: {config_json_file}')

        with open(config_json_file, mode='r', encoding='utf-8') as json_file:
            try:
                vgpu_json = json.load(json_file)
            except json.JSONDecodeError as exc:
                logger.error("Invalid VMTB config JSON format: %s", exc)
                raise exceptions.VmtbConfigError(f'Invalid VMTB config JSON format: {exc}')

        return vgpu_json

    def get_igt_config(self, igt_config_json: Dict) -> VmtbIgtConfig:
        igt_config = VmtbIgtConfig(
            test_dir=igt_config_json['igt']['test_dir'],
            tool_dir=igt_config_json['igt']['tool_dir'],
            lib_dir=igt_config_json['igt']['lib_dir'],
            result_dir=igt_config_json['igt']['result_dir'],
            options=igt_config_json['igt']['options'])

        return igt_config

    def parse_json_file(self, config_json: Dict) -> VmtbConfig:
        vmtb_host_config = VmtbHostConfig(
            card_index=config_json['host']['card_index'],
            driver=config_json['host']['driver'],
            igt_config=self.get_igt_config(config_json['host']))

        vmtb_guest_config = VmtbGuestConfig(
            os_image_path=config_json['guest']['os_image'],
            driver=config_json['guest']['driver'],
            igt_config=self.get_igt_config(config_json['guest']))

        vmtb_config = VmtbConfig(
            host_config=vmtb_host_config,
            guest_config=vmtb_guest_config,
            vgpu_profiles_path=config_json['resources']['vgpu_profiles_path'],
            wsim_wl_path=config_json['resources']['wsim_wl_path'],
            guc_ver_path=config_json['resources']['guc_ver_path'],
            ci_host_dmesg_file=config_json['ci']['host_dmesg_file'])

        return vmtb_config
