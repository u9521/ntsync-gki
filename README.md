# ntsync-gki

`ntsync-gki` packages the Linux NT synchronization driver as one KernelSU module for arm64 Android GKI kernels. The archive contains a `gki_ntsync.ko` module for each supported KMI. Its installer detects `uname -r`, unpacks only the matching module into the installed module directory, and removes the remaining payloads.

At boot, the module calls `ksud insmod`. This KernelSU loader resolves relocations through kallsyms, so GKI symbols do not need to appear in the normal exported-symbol table. The build intentionally sets `KBUILD_MODPOST_WARN=1` for that reason.

## Requirements

- An arm64 GKI device running KernelSU with `ksud insmod` kallsyms relocation support.
- KernelSU Manager for installation. Recovery and Magisk installation are not supported.
- A kernel matching one of the KMI patterns below. Vendor kernels with a similar version string are not guaranteed compatible.

| DDK target | KMI pattern |
| --- | --- |
| `android12-5.10` | `5.10.*-android12-*` |
| `android13-5.10` | `5.10.*-android13-*` |
| `android13-5.15` | `5.15.*-android13-*` |
| `android14-5.15` | `5.15.*-android14-*` |
| `android14-6.1` | `6.1.*-android14-*` |
| `android15-6.6` | `6.6.*-android15-*` |
| `android16-6.12` | `6.12.*-android16-*` |

## Install

1. Download the release zip and install it from KernelSU Manager.
2. The installer rejects non-KernelSU, non-arm64, and unsupported KMI devices.
   A package containing `gki_ntsync.ko` at its root bypasses the architecture and KMI checks and installs that driver directly.
3. Reboot. The `post-fs-data` or KernelSU late-load hook invokes `ksud insmod`.

After boot, verify `/sys/module/gki_ntsync` and `/dev/ntsync`. If loading fails, inspect `/data/adb/modules/ntsync-gki/gki_ntsync.log` and the kernel log. Each entry in `gki_ntsync.log` starts with a date and time and records the loading checks, result, and `ksud insmod` output.

The module's Action button in KernelSU Manager toggles the driver state: it loads an unloaded driver or runs `rmmod gki_ntsync` for a loaded driver. The Action script prints its result in the Manager and does not write to `gki_ntsync.log`.

Uninstalling removes the boot script and module file. Reboot after uninstall to remove a module already loaded in the current kernel.

## Development

Build a local target with an installed DDK:

```sh
make -C driver KDIR=/opt/ddk/kdir/android15-6.6
```

Or use the DDK Docker helper from the repository root. Passing `-C driver` keeps the reference source directory mounted:

```sh
ddk build --target android15-6.6 -- -C driver
```

To package all built modules, arrange them as `build/modules/<target>/gki_ntsync.ko`, then run:

```sh
uv run python tools/package_module.py \
  --modules-dir build/modules \
  --version 1.0.0 \
  --output dist/ntsync-gki-v1.0.0.zip
```

`config/targets.json` is the single source of truth for the CI matrix, payload names, and installer KMI selection logic.

## CI

GitHub Actions builds every target in its matching `ghcr.io/ylarod/ddk` container, runs the packaging tests, and uploads one module zip artifact. Pushing a `vMAJOR.MINOR.PATCH` tag also creates a GitHub Draft Release.

## Source provenance

The driver implementation is vendored at `driver/mainline/ntsync.c`, licensed GPL-2.0-only. `driver/gki_ntsync.c` adds the minimal Android GKI, 5.10 compatibility, and KernelSU device-node integration.
