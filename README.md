# Lindroid kernel — display control for Poco X5 5G / Redmi Note 12 5G

Custom display-control ioctls for the `evdi-lindroid` virtual display, on top
of the Darkmoon-Reborn kernel that the shipped ROM already uses.

## Why Darkmoon and not the Protium fork

Both forks sit on 5.4.302, but only Darkmoon works with this ROM.

Protium boots and the display comes up, but Android then starts in a degraded
state: the UI is wrong, the lock screen and wallpapers are gone, and it looks
like a fresh setup. Verified by building Protium both with and without the
Lindroid driver — the same broken UI either way, so the driver is not at fault.
Stock kernel on the same ROM is fine.

The config diff explains it. Darkmoon's `stone_defconfig` carries four options
Protium has none of:

```
ANDROID_VENDOR_HOOKS=y    CFI_CLANG=y
MODVERSIONS=y             SHADOW_CALL_STACK=y
```

`ANDROID_VENDOR_HOOKS` is the important one: without it the vendor HAL cannot
attach to the kernel. Darkmoon's defconfig matches the config embedded in the
shipped kernel 729 lines out of 742 (the remainder are Kbuild defaults that
never appear in a defconfig).

## What the ioctls are for

The physical panel is scanned out by Android userspace, so a Linux process
cannot program the display hardware directly. What it *can* do is drive the
EVDI virtual display that the Lindroid composer feeds to the panel. These
ioctls are the switch:

- **owner** — `android` (Linux stops presenting) or `linux`
- **screen** — `on` / `off`

While `owner != linux` or `screen != off`, swap events stop and the panel
shows Android's own UI, or nothing.

`DRM_IOCTL_EVDI_CONTROL` gets/sets the state, `DRM_IOCTL_EVDI_GET_INFO` reports
one display in full, `DRM_IOCTL_EVDI_WAIT_EVENT` blocks for 5 s on a change.
Defined in `drivers/lindroid-drm/uapi/evdi_drm.h` at command numbers 0x10–0x12,
right after the existing EVDI ioctls — nothing already shipped is touched.

See [docs/display-control.md](docs/display-control.md) for the full interface.

## Build

```sh
./setup.sh              # clone base, KernelSU-Next, patches, driver
cd ~/dm-kernel
./build.sh Lindroid     # produces Lindroid-<date>.zip
```

Flashable zip goes through AnyKernel3, writes `boot` only, and ships the
kernel alone — no `dtb`/`dtbo`. That is deliberate; see below.

## Things that will bite you

Each of these cost real debugging time. They are load-bearing.

**Do not let the driver create a DRM card at boot.** DRM minors are assigned in
registration order and the Qualcomm display driver registers first. A card
created at init claims `/dev/dri/card0` and pushes the real panel to `card1`.
Android hardcodes `card0`, so SurfaceFlinger never starts and the phone hangs
on the boot logo with adb still working. Hence `EVDI_DEFAULT_NR_DEVICES = 0`,
and a regression test for it in `tools/test-control.c`.

**Do not put `dtb` or `dtbo` in the zip.** The stock `boot.img` carries no
appended DTB, and `ak3-core.sh`'s `flash_boot` silently appends any `dt`/`dtb`/
`recovery_dtbo` it finds in the zip root. Shipping one changes how the panel
initialises, and Android again comes up in a degraded state.

**The upstream tree has no build script and needs `mkdtimg`, which is gone.**
`dtbo.img` is dropped from the build targets in patch 0001 — it would only be
thrown away, and it cannot be produced anyway.

**Toolchain skew.** The shipped kernel is built with clang 21. Building with a
newer clang turns implicit enum-to-enum conversions into errors, and the
out-of-tree `qcacld-3.0` WiFi driver has many. `build.sh` silences exactly
that one diagnostic.

**Use the `legacy` KernelSU-Next branch.** The current one requires 5.10+.

## Verified on device

```
Linux localhost 5.4.302-Darkmoon-Reborn #1 SMP PREEMPT Mon Apr 6 17:56:23 CEST 2026
```

`card0` stayed with `mdss_mdp`; the EVDI card came up as `card1` when created
on demand. `owner` and `screen` both read and write correctly through the
ioctls, with Android running normally throughout.

Build `lindroid-ctl` with `-static` — Android has no
`/lib/ld-linux-aarch64.so.1`, so a dynamically linked binary will not run:

```sh
clang --target=aarch64-linux-gnu -static -O2 -I. -I../uapi -idirafter <k>/include/uapi lindroid-ctl.c -o lc
```

## What is not done

The buffer exchange needs a partner on the Android side. Swap events are only
generated for buffers that carry a `gralloc_buf_id`, which is assigned through
the EVDI create/get-buffer protocol — so a bare compositor in a chroot renders
into buffers that never reach the panel. The full path is
`create-disp` (container) ↔ EVDI (this driver) ↔ `vendor.lindroid.composer`
(Android, over binder).

On top of that, the Lindroid composer has to be picked up as SurfaceFlinger's
hardware composer, which means the ROM has to look for the
`vendor.lindroid.composer` AIDL service rather than the standard
`android.hardware.graphics.composer3.IComposer/default`. That is a ROM-side
change, not a kernel one.

The kernel half is complete and tested; the userspace half is not wired up.
