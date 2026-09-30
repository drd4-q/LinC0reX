# Taking the panel from Android

Linux can drive the physical panel on this phone. Not through a compositor,
not through the GPU, and not through the EVDI virtual display: a plain
userspace process in a chroot becomes DRM master on `card0` and programs the
DSI panel directly.

This is the recipe that works, and the two things that make it work that are
easy to get wrong.

## What has to be stopped, and what that actually is

Two services, not one. Both hold the display:

```
setprop ctl.stop surfaceflinger
setprop ctl.stop vendor.qti.hardware.display.composer
```

The second one is the important one. SurfaceFlinger is the obvious suspect,
but on this device the panel is scanned out by the Qualcomm composer HAL, an
independent service that opens `card0` itself. With only SurfaceFlinger
stopped, `composer-servic` still holds six descriptors on the card and no
other client can take master. Check who is holding it with:

```
for p in /proc/[0-9]*; do
  for f in $p/fd/*; do
    t=$(readlink "$f" 2>/dev/null)
    case "$t" in *card0*) echo "$(cat $p/comm 2>/dev/null)";; esac
  done
done | sort -u
```

With both stopped, `card0` is free and `drmSetMaster` succeeds.

## The chroot

A Debian bookworm arm64 rootfs under `/data/lindroid/droot`. It is built on
the workstation with `tools/fetchdeb.py`, which resolves dependencies from the
Packages index and unpacks them directly, because debootstrap requires root
that is not available there.

Packages that matter: `weston`, `libweston-10-0` (the DRM backend is here,
not in `weston`), `libdrm-tests` (modetest), `drm-info`.

Merged-usr has to be finished by hand. debootstrap dies before creating the
symlinks, leaving `/lib` a real directory, and then nothing that is
dynamically linked will start:

```
cp -a droot/lib/aarch64-linux-gnu droot/usr/lib/
mv droot/lib/udev droot/usr/lib/udev
rm -rf droot/lib && ln -s usr/lib droot/lib
```

The backend modules also have to be linked into the directory weston actually
searches:

```
ln -sf ../libweston-10/drm-backend.so droot/usr/lib/aarch64-linux-gnu/weston/
```

Mounts needed in the chroot, or `/dev/dri` will be empty inside it:

```
mount -t proc proc $R/proc
mount -o bind /dev $R/dev
mount -o bind /sys $R/sys
```

## The udev database

Android has no udev, so `/run/udev/data` does not exist. libdrm resolves
device names through libudev, which reads that database, so enumeration comes
back empty even though `/dev/dri/card0` is right there and `open()` on it
works. Both `modetest` and weston fail this way, with "no drm device found".

Hand-building the two entries is enough:

```
mkdir -p $R/run/udev/data

cat > $R/run/udev/data/n226:0 <<EOF
E:DEVPATH=/devices/platform/soc/5e00000.qcom,mdss_mdp/drm/card0
E:DEVNAME=/dev/dri/card0
E:DEVTYPE=drm_minor
E:MAJOR=226
E:MINOR=0
E:SUBSYSTEM=drm
E:USEC_INITIALIZED=1000000000
E:ID_PATH=platform-soc-5e00000.qcom,mdss_mdp-dri
E:ID_PATH_TAG=5e00000.qcom-mdss_mdp
G:seat
EOF
```

`n226:128` is the same thing for `renderD128`. Take the path, major, minor and
subsystem from `/sys/class/drm/card0/uevent`.

With this, `udev-probe` reports `DEVNAME=/dev/dri/card0` and `drmprobe` gets a
usable node back from `drmGetDevices2`.

It still does not make weston work on its own, because the way weston was
being invoked was wrong. See below — that part turned out to be an argument
error, not a driver or libdrm problem.

## weston, and the argument that wasted the evening

`weston` failed to start for a long time with:

```
Trying direct launcher...
ERROR: could not open DRM device '/dev/dri/card0'
no drm device found
```

That message reads like a device discovery failure, and the empty string that
libdrm returns in `node[1]` for the missing control node looks like the
culprit. It is not. The actual path, in `libweston/backend-drm/drm.c`:

```c
static struct udev_device *
open_specific_drm_device(struct drm_backend *b, const char *name)
{
	device = udev_device_new_from_subsystem_sysname(b->udev, "drm", name);
	if (!device) {
		weston_log("ERROR: could not open DRM device '%s'\n", name);
		return NULL;
	}
```

`name` is `config->specific_device`, assigned straight from the command line
in `compositor/main.c` with no path stripping anywhere in between. So passing
`--drm-device=/dev/dri/card0` makes libudev search for a sysfs entry whose
sysname is the string `/dev/dri/card0`, which does not exist.

The help text says it plainly, and it was read past twice:

```
--drm-device=CARD   The DRM device to use, e.g. "card0".
```

The correct invocation is the sysname, not the path:

```
weston --backend=drm-backend.so --drm-device=card0 --renderer=noop
```

`udev_device_get_devnode()` then needs the `/run/udev/data` entry built above,
so the database is still needed — but for the devnode lookup, not because
enumeration is broken.

Note also that `--backend` takes a module filename, not a backend name:
`drm-backend.so`, not `drm`. That one really is a weston quirk, and it does
produce a misleading "unknown backend" error.

## weston: how far it gets

With `--drm-device=card0` the device is found and weston proceeds:

```
using /dev/dri/card0
DRM: supports atomic modesetting
DRM: supports GBM modifiers
Loading module '.../libweston-10/gl-renderer.so'
... GL ES 3.2 - renderer features
```

Mesa initialises on llvmpipe — software rendering, as intended for a first
pass. Getting there needed `libgl1-mesa-dri` (which is what the package is
called; `mesa-dri-drivers` is a virtual name), plus its transitive
`libLLVM-15`, `libsensors5`, `libedit2` and friends. Mesa looks in
`/usr/lib/dri`, not only in the multiarch path, so the drivers have to be in
both.

Then weston 10 aborts:

```
weston: ../libweston/drm-formats.c:131: weston_drm_format_array_add_format:
Assertion `!weston_drm_format_array_find_format(formats, format)' failed.
```

This is an upstream weston bug, not a driver or kernel problem.
`drm_plane_populate_formats()` in `libweston/backend-drm/kms.c` walks a
plane's `IN_FORMATS` blob and adds each entry with no duplicate check, and SDE
advertises the same fourcc twice. `weston_drm_format_array_join()` a few lines
below does check, so the invariant is real, just not enforced consistently.

`tools/fmtshim.so` intercepts both `weston_drm_format_array_add_format` and
`weston_drm_format_add_modifier` and returns the existing entry instead of
asserting. Both are needed: with only the first, the second assertion fires
immediately. The duplicate carries a different modifier, so returning the
existing entry is not the same as dropping data.

Whether weston then runs to completion is **not yet established**. The
interception is confirmed to work — weston gets past `drm-formats.c:131` — but
the run was cut short before the end.

## The actual test

```
# chroot /data/lindroid/droot /usr/bin/kmsclaim
открыл /dev/dri/card0 (fd=3)
стал DRM master
CRTC=1 коннекторов=1 кодировщиков=1
  коннектор 32: подключён, режимов 6
  выбран коннектор 32: 1080x2400
  возможные CRTC: 0x1
  CRTC 76: текущий fb=0, 0x0 (режим невалиден)
  плоскостей: 3
  dumb buffer 1080x2400 stride=4352 size=10444800
  framebuffer 99

== legacy drmModeSetCrtc ==
*** LEGACY MODESET OK ***
```

The screen shows eight vertical colour bars. `card0-DSI-1/status` reads
`connected` and `enabled`.

`kmsclaim` then holds the panel in a `pause()` loop on purpose: DRM master is
released when the fd closes, so the process staying alive *is* the mechanism
for keeping the screen.

## Handing it back

```
pkill kmsclaim
setprop ctl.start vendor.qti.hardware.display.composer
setprop ctl.start surfaceflinger
```

Order matters on the way back too. Start the composer before SurfaceFlinger,
or SurfaceFlinger comes up against a display nothing owns.

## What is still missing

A working compositor configuration, and input. `weston` should start once
invoked as above, but that is untested on device because the phone was
disconnected before the argument was found. The next things to establish, in
order:

- Confirm weston starts and takes the panel.
- Input. Nothing is passed through to the chroot but `/dev` and `/sys` right
  now, and a bind-mounted `/dev` does include `/dev/input`, so touch should
  already be visible. Worth checking, because without it phosh is unusable
  however well the display works.
- GPU. KGSL exposes no DRM node, so neither Mesa nor freedreno can see the
  Adreno 619. Software rendering means llvmpipe, which is slow but adequate
  to prove the stack. The test image in `kmsclaim` is a CPU dumb buffer for the
  same reason.

The EVDI driver is no longer needed for any of this and could be dropped, but
it is harmless and already wired in, so it is not worth the churn right now.
