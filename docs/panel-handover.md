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

It still does not make weston work, though. This device has no control node,
so libdrm returns an empty string in `node[1]` rather than NULL; opening it
gives ENOENT and weston concludes there is no device. The panel and the kernel
are fine. `kmsclaim` sidesteps the whole path by opening `/dev/dri/card0`
directly.

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

A compositor. Software rendering is available in principle but weston cannot
be made to run on this driver without patching its device discovery, and the
obvious fix — an `LD_PRELOAD` shim over `drmGetDevices2` that patches up the
node list, or passing the fd in from a launcher — is untried. A minimal
compositor built directly on libdrm, the way `kmsclaim` is, is the fallback
and has the advantage of already being proven to work.

GPU support is a separate problem. KGSL exposes no DRM node, so neither Mesa
nor freedreno can see the Adreno 619, which is why the test image is a CPU
dumb buffer.
