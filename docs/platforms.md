# :material-check-circle: Platforms

## :material-chip: Hardware

**IGT GPU Tools** is designed to validate few DRM drivers across a variety of platforms.
While virtual environments help with development, real hardware is recommended for full
test coverage, performance metrics, and regression tracking.

### :material-lightbulb-outline: General Guidelines

- **Use modern GPUs** for maximum test compatibility.
- Prefer **dedicated test systems** for kernel and firmware flexibility.
- Ensure **reliable cooling** to avoid throttling during performance tests.

### Supported GPUs

Support exists for the following platforms:

- Intel (i915 and xe)
- AMD (amdgpu)
- NVIDIA (nouveau)
- Broadcom (v3d and vc4)
- Qualcomm (msm)
- Arm (Panfrost)
- Panthor
- Virtual GPUs (e.g., virtio_gpu in QEMU/KVM/AVD or vmwgfx)
- Virtual display (vkms)

#### Intel

| GPU Generation | Example Platform / CPU      | Notes                                        |
|-||-|
| Gen9           | Skylake (e.g., i5-6200U)    | Stable, well-supported baseline              |
| Gen11          | Ice Lake (e.g., i7-1065G7)  | Better display/media support                 |
| Gen12          | Tiger Lake / Alder Lake     | Used for both `i915` and early `Xe` testing  |
| DG2            | Intel Arc A-Series GPUs     | Required for `Xe` HPG-specific validation    |
| XeHP/XeHPC     | (If available)              | Specialized use by hardware teams/devs       |

:material-information-outline: *Older GPUs (Gen7/Gen8) are still useful for regression
testing but may not support new test features.*


:material-account-group-outline: Community contributions are welcome to expand non-Intel
support!

## :material-script: Software

### Intel

#### :material-shield-account-outline: i915 vs Xe Drivers

IGT supports two major Intel GPU drivers in the kernel:

Here are some basic information, for more please visit an official
[Intel Graphics for Linux - Documentation](https://drm.pages.freedesktop.org/intel-docs/)

##### :material-chip: `i915` Driver

- Supports older Intel platforms (Gen9–Gen12).
- Mature, well-tested, and fully integrated into kernel and userspace stacks.
- Tests targeting `i915` must include proper documentation (using `igt_describe`) and
  follow test plan validation rules.

##### :material-flash: `Xe` Driver

- Next-generation Intel GPU driver for **DG2 (Arc)** and newer architectures.
- Designed to replace `i915` for future platforms.
- Modular, uses a new codebase and device model.
- Tests targeting `Xe` must follow the same documentation and validation practices but may
  focus on newer UAPI/ABI interactions.

#### :material-alert-outline: Key Differences

| Aspect           | i915                         | Xe                                    |
|||-|
| Target Platforms | Gen9–Gen12, some DG1         | DG2 (Arc), XeHP/HPG/HPC               |
| Kernel Interface | Legacy DRM, widely used      | New driver stack with modernized UAPI |
| Development      | Stable, long-maintained      | In active development                 |
| Test Requirements| Strict documentation rules   | Same, with new feature emphasis       |

If you're unsure which driver your hardware uses, check:

```bash
lspci -nn | grep VGA
dmesg | grep drm
```
