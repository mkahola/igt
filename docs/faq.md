# Frequently Asked Questions (FAQ)

This FAQ covers common questions about using, building, and contributing to
**IGT GPU Tools**.

### What is IGT GPU Tools?

IGT GPU Tools is a collection of low-level tests and utilities used to validate and
debug Linux DRM (Direct Rendering Manager) graphics drivers. It focuses primarily
on display testing. It supports GPU-specific testing for Intel and AMD.
Other platforms are supported in varying degrees.

### How do I build IGT?

You can build IGT using [Meson](https://mesonbuild.com/) and Ninja. Here's a quick
example:

```sh
meson setup build && ninja -C build
```

Make sure required dependencies are installed. You can refer to:

- `Dockerfile.build-fedora`
- `Dockerfile.build-debian` or `Dockerfile.build-debian-minimal`

They contain up-to-date package lists for common distros.

### How do I run tests?

Tests are located in the `tests/` directory and can be run directly. For example:

```sh
sudo ./build/tests/core_auth
```

Use `--list-subtests` to list available subtests and `--run-subtest` to run a specific
one:

```sh
sudo ./build/tests/core_auth --list-subtests
sudo ./build/tests/core_auth --run-subtest basic-auth
```

You can also run tests using the `scripts/run-tests.sh` wrapper, which supports
filtering and batch execution.

### Do I need to run as root?

Most tests require root privileges and a system without a running graphical session
(X or Wayland). Some tools may work without root, especially those that only inspect
or decode state.

### What platforms are supported?

IGT primarily targets platforms:

- Intel (i915 and xe)
- AMD (amdgpu)
- NVIDIA (nouveau)
- Broadcom (v3d and vc4)
- Qualcomm (msm)
- Arm (Panfrost)
- Panthor
- Virtual GPUs (e.g., virtio_gpu in QEMU/KVM/AVD or vmwgfx)
- Virtual display (vkms)

Hardware coverage may vary by test.

### What's the difference between `tests/`, `tools/`, and `benchmarks/`?

- `tests/` – Automated functional tests, designed for CI and driver validation
- `tools/` – Debugging and inspection utilities (e.g., checking GPU state)
- `benchmarks/` – Performance-oriented microbenchmarks (e.g., memory or rendering speed)

### Where do I report security issues?

Do **not** report vulnerabilities in public issues or mailing lists.
Instead, contact a maintainer directly. See the `MAINTAINERS` file for contact
information.

### How do I submit a patch?

Use `git send-email` to send your patch to:

```
igt-dev@lists.freedesktop.org
```

Prefix your subject with `PATCH i-g-t`. You can track submissions at:

- [Patchwork](https://patchwork.freedesktop.org/project/igt/series/)
- [Lore Archive](https://lore.kernel.org/igt-dev/)

More details are in [CONTRIBUTING.md](CONTRIBUTING.md).

If your question isn't answered here, feel free to ask on the
[igt-dev mailing list](mailto:igt-dev@lists.freedesktop.org) or open an issue on GitLab.

