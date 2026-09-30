# Lindroid display control

Kernel-side support for driving the phone display from a normal Linux
userspace, so a Wayland compositor running Phosh or GNOME Mobile can take the
physical panel over from Android and hand it back.

## Why the panel cannot be programmed from Linux directly

There is no `CONFIG_DRM_MSM` in this kernel. The physical panel is scanned out
by the Android userspace display HAL, not by a DRM driver, so a Linux process
cannot reprogram the display hardware the way a desktop DRM client would.

What Linux *can* do is drive the `evdi-lindroid` virtual display, which the
Lindroid composer hands to the panel. This is the same path the desktop already
uses, with an explicit switch on top of it.

## The switch

Two pieces of state, per display:

- **owner** — which side currently feeds the panel. `linux` (default) or
  `android`.
- **screen** — whether the display is lit. `on` (default) or `off`.

While `owner != linux` or `screen != off`, the driver stops delivering swap
events to the composer, so nothing from EVDI reaches the panel and Android's own
UI (or nothing at all) is what you see.

The default is `owner = linux`, which is exactly how the driver behaved before
these ioctls existed. A freshly booted display keeps feeding the panel; handing
it back is always an explicit act.

The handover is two-sided so neither side can get stuck:

1. Linux sets `owner = linux` — EVDI starts presenting.
2. The Android side notices, drops its own layers, and calls
   `EVDI_CTL_ACK_OWNER` to record that the switch actually happened.

`acked_owner` is visible in `status` and sysfs, so a host script can tell
"Linux asked" from "Android agreed".

## Sysfs

Exposed on the DRM device once it is registered. The platform device is
parented to the driver's own root device, not to `/sys/devices/platform`, so
the path is:

```
/sys/devices/evdi-lindroid/evdi-lindroid.0/
```

The driver logs the exact path at probe time.

```
cat  /sys/devices/evdi-lindroid/evdi-lindroid.0/owner   # android | linux
echo linux   > .../owner
cat  .../screen                                         # on | off
echo off     > .../screen
cat  .../state                                          # per-display dump
```

The path is printed by the driver at probe time and also logged as
"Display control available at /sys/devices/platform/evdi-lindroid.N/".

`owner` and `screen` are mode 0666, matching the existing `add` attribute, so
the unprivileged container-side daemon can reach them without extra udev rules.
`state` is read-only (0444).

## Ioctls

Defined in `uapi/evdi_drm.h`, all three take a `struct drm_evdi_control` /
`struct drm_evdi_display_info` and are registered at command numbers 0x10–0x12,
right after the pre-existing EVDI ioctls. Nothing that already shipped on the
Android side is touched.

| ioctl | nr | purpose |
|---|---|---|
| `DRM_IOCTL_EVDI_CONTROL` | 0x10 | get/set owner, screen, power mode, ack, geometry |
| `DRM_IOCTL_EVDI_GET_INFO` | 0x11 | full state of one display |
| `DRM_IOCTL_EVDI_WAIT_EVENT` | 0x12 | block until the owner/state changes |

`DRM_IOCTL_EVDI_WAIT_EVENT` takes a 5 second timeout and returns `-ETIMEDOUT`,
so a host process can wait on state changes without spinning. Pass
`display_id = -1` (or 0) and `arg = <owner>` to wait for a specific owner.

## lindroid-ctl

`drivers/lindroid-drm/tools/lindroid-ctl.c` is a small client that finds the
evdi card by matching `DRIVER=evdi-lindroid` under `/sys/class/drm/*/device/uevent`.

```
make -C drivers/lindroid-drm/tools
lindroid-ctl status
lindroid-ctl to-linux      # hand the panel to Linux
lindroid-ctl to-android    # give it back
lindroid-ctl screen off
lindroid-ctl wait linux
```

## Boot-time device creation — deliberately off

`evdi.evdi_nr_devices=` controls how many displays are created at driver init.
The default is **0**, and it must stay that way.

DRM minors are assigned in registration order. The Qualcomm display driver
registers before this driver does, so creating the EVDI card at boot makes it
claim `/dev/dri/card0` and pushes the real panel to `card1`. Android hardcodes
`card0` in SurfaceFlinger and gralloc, so the result is a phone stuck on the
boot logo with no SurfaceFlinger running, while adb still works.

This was observed on moonstone, where the panel is `card1` when the EVDI card
is `card0`:

```
card0 -> ../../devices/evdi-lindroid/evdi-lindroid.0/drm/card0
card1 -> ../../devices/platform/soc/5e00000.qcom,mdss_mdp/drm/card1
```

Nothing is lost by leaving it off: the container-side `create-disp` daemon
already writes to `/sys/devices/evdi-lindroid/add` when it needs a card, and
that path was the upstream behaviour before.

## Caveats

- The panel is still programmed by Android. If the Android composer is not
  running, setting `owner = linux` gives a black screen, not a Linux desktop.
- `acked_owner` only advances if the Android side is built to send
  `EVDI_CTL_ACK_OWNER`. Without that support the request still gates EVDI
  presentation, you just cannot observe the confirmation.
- The old DisplayLink EVDI `EVDI_SET_POWER_MODE` ioctl and the new
  `EVDI_CTL_SET_POWER_MODE` command both write the same field, and the same
  applies to the two notification mechanisms.
