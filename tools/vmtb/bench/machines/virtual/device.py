# SPDX-License-Identifier: MIT
# Copyright © 2025-2026 Intel Corporation

import logging
import re
import typing

from bench import exceptions
from bench.configurators import pci
from bench.drivers.driver_interface import DriverInterface, VfDriver
from bench.machines.device_interface import DeviceInterface
from bench.machines.machine_interface import MachineInterface

logger = logging.getLogger('VirtualDevice')


class VirtualDevice(DeviceInterface):
    class PciInfo:
        def __init__(self, bdf: str, vm: MachineInterface) -> None:
            self.bdf: str = bdf
            self.vm: MachineInterface = vm
            self.devid: str = self.get_device_id(self.bdf)
            self.minor_number: int = self.get_device_minor_number(self.bdf)

        def get_device_minor_number(self, bdf: str) -> int:
            drm_dir = f'/sys/bus/pci/devices/{bdf}/drm'

            for dir_name in self.vm.dir_list(drm_dir):
                if 'card' in dir_name:
                    index_match = re.match(r'card(?P<card_index>\d+)', dir_name)
                    if index_match:
                        return int(index_match.group('card_index'))

            logger.error("Could not determine minor number (card index) for device %s", bdf)
            raise exceptions.HostError(f'Could not determine minor number (card index) for device {bdf}')

        def get_device_id(self, bdf: str) -> str:
            device_file = f'/sys/bus/pci/devices/{bdf}/device'
            devid = self.vm.read_file_content(device_file)

            return devid.strip()[2:] # Strip whitespaces and 0x

    def __init__(self, bdf: str, vm: MachineInterface) -> None:
        self.pci_info = self.PciInfo(bdf, vm)
        self.gpu_model = pci.get_gpu_model(self.pci_info.devid)
        # Reference to VirtualMachine - required for Guest-Agent use (e.g. guest file read/write):
        self.vm: MachineInterface = vm
        self.driver: DriverInterface = VfDriver(self.pci_info.bdf, self.vm)

        # Resources provisioned to the VF/VM:
        self._lmem_size: typing.Optional[int] = None
        self._ggtt_size: typing.Optional[int] = None
        self._contexts: typing.Optional[int] = None
        self._doorbells: typing.Optional[int] = None
        # Tile mask is relevant for multi-tile devices:
        self._tile_mask: typing.Optional[int] = None

    def __str__(self) -> str:
        return f'VirtualDevice-{self.pci_info.bdf}'

    def bind_driver(self) -> None:
        logger.debug("Bind %s driver to virtual device - PCI BDF: %s",
                     self.vm.get_drm_driver_name(), self.pci_info.bdf)
        self.driver.bind()

    def unbind_driver(self) -> None:
        logger.debug("Unbind %s driver from virtual device - PCI BDF: %s",
                     self.vm.get_drm_driver_name(), self.pci_info.bdf)
        self.driver.unbind()

    @property
    def lmem_size(self) -> typing.Optional[int]:
        if self._lmem_size is None:
            self.get_debugfs_selfconfig()

        return self._lmem_size

    @property
    def ggtt_size(self) -> typing.Optional[int]:
        if self._ggtt_size is None:
            self.get_debugfs_selfconfig()

        return self._ggtt_size

    @property
    def contexts(self) -> typing.Optional[int]:
        if self._contexts is None:
            self.get_debugfs_selfconfig()

        return self._contexts

    @property
    def doorbells(self) -> typing.Optional[int]:
        if self._doorbells is None:
            self.get_debugfs_selfconfig()

        return self._doorbells

    @property
    def tile_mask(self) -> typing.Optional[int]:
        if self._tile_mask is None:
            self.get_debugfs_selfconfig()

        return self._tile_mask

    def get_num_gts(self) -> int:
        """Get number of GTs for a device based on exposed sysfs gt nodes."""
        gt_num = 0
        # Fixme: tile0 only at the moment, add support for multiple tiles if needed
        path = self.driver.sysfs_device_path / 'tile0'

        if self.vm.dir_exists(str(path / 'gt')):
            gt_num = 1
        else:
            while self.vm.dir_exists(str(path / f'gt{gt_num}')):
                gt_num += 1

        return gt_num

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

    # __parse_selfconfig_size - parses size string from debugfs/self_config
    # GGTT/LMEM size on xe has the following format:
    # GGTT size:      536870912 (512 MiB)
    # whereas on i915 (unit suffix):
    # GGTT size:     524288K
    # @size_str: size string read from self_config file
    # (with unit suffix (B/K/M/G) or in bytes (without suffix))
    # Returns: size in bytes
    def __parse_selfconfig_size(self, size_str: str) -> int:
        retval = 0
        size_match = re.search(r'(?P<size_units>^\d+[BKMG])|(?P<size_bytes>^\d+)', size_str.strip())
        if size_match:
            if size_match.group('size_units'):
                # i915 specific format (in B/K/M/G units)
                retval = self.helper_convert_units_to_bytes(size_match.group('size_units'))
            elif size_match.group('size_bytes'):
                # Xe specific format (in Bytes)
                retval = int(size_match.group('size_bytes'))
            else:
                logger.warning("Unexpected size pattern match!")

        return retval

    def get_debugfs_selfconfig(self, gt_num: int = 0) -> None:
        """Read hard resources allocated to VF from debugfs selfconfig file."""
        out = self.driver.read_debugfs(f'gt{gt_num}/vf/self_config')

        for line in out.splitlines():
            param, value = line.split(':')

            if param == 'GGTT size':
                self._ggtt_size = self.__parse_selfconfig_size(value)
            elif param == 'LMEM size':
                self._lmem_size = self.__parse_selfconfig_size(value)
            elif param.find('contexts') != -1:
                self._contexts = int(value)
            elif param.find('doorbells') != -1:
                self._doorbells = int(value)
            elif param == 'tile mask':
                self._tile_mask = int(value, base=16)
