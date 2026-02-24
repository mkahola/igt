# SPDX-License-Identifier: MIT
# Copyright © 2024-2026 Intel Corporation

import enum
import typing


class GpuModel(str, enum.Enum):
    PTL = 'Panther Lake (PTL)'
    BMG = 'Battlemage (BMG)'
    Unknown = 'Unknown'

    def __str__(self) -> str:
        return str.__str__(self)


def get_gpu_model(pci_id: str) -> GpuModel:
    """Return GPU model associated with a given PCI Device ID."""
    return pci_ids.get(pci_id.upper(), GpuModel.Unknown)


def get_vgpu_profiles_file(gpu_model: GpuModel) -> str:
    """Return vGPU profile definition JSON file for a given GPU model."""
    if gpu_model == GpuModel.PTL:
        vgpu_device_file = 'Ptl.json'
    elif gpu_model == GpuModel.BMG:
        vgpu_device_file = 'Bmg_g21_12.json'
    else: # GpuModel.Unknown
        vgpu_device_file = 'N/A'

    return vgpu_device_file


# PCI Device IDs: PTL
_ptl_pci_ids = {
    'B080': GpuModel.PTL,
    'B081': GpuModel.PTL,
    'B082': GpuModel.PTL,
    'B083': GpuModel.PTL,
    'B084': GpuModel.PTL,
    'B085': GpuModel.PTL,
    'B086': GpuModel.PTL,
    'B087': GpuModel.PTL,
    'B08F': GpuModel.PTL,
    'B090': GpuModel.PTL,
    'B0A0': GpuModel.PTL,
    'B0B0': GpuModel.PTL,
    'FD80': GpuModel.PTL,
    'FD81': GpuModel.PTL
}


# PCI Device IDs: BMG (G21 - VRAM: 12GB / other)
_bmg_pci_ids = {
    'E20B': GpuModel.BMG # B36 / 12GB
}


# All PCI Device IDs to GPU Device Names mapping
pci_ids: typing.Dict[str, GpuModel] = {**_ptl_pci_ids,
                                       **_bmg_pci_ids}
