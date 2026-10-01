# weston on the phone

A Wayland compositor runs on the phone's physical display, with touch input
and software rendering. This is what it took.

```
lindroid weston     # take the panel, start weston
lindroid back       # give it back to Android
```

A client then connects to the socket weston reports:

```
lindroid run env WAYLAND_DISPLAY=wayland-1 weston-terminal
```

`wayland-info` against it reports the panel as a normal output:

```
interface: 'zxdg_output_manager_v1',  version: 2
        output: 15
        name: 'DSI-1'
        logical_width: 1080, logical_height: 2400
interface: 'wl_shm',  version: 1
```

## The udev database, and the one character that broke it

Android has no udev, so `/run/udev/data` does not exist. libdrm resolves
device names through libudev and libinput finds input devices the same way, so
without that database neither enumerates anything.

Entries are keyed `c<major>:<minor>` for character devices. `n<major>:<minor>`
is the old udev-only name and this libudev does not read it.

That distinction cost the most time here, because a wrongly-named entry is
ignored *silently* and still looks partly working: `DEVNAME` comes back either
way, since systemd derives it from the sysfs `uevent` file and adds the `dev/`
prefix itself. Only `ID_PATH`, `ID_SEAT`, `USEC_INITIALIZED` and the `I:`
marker go missing. `ID_PATH` is the reliable probe - it cannot come from sysfs,
so a value there means the database was read.

What `I:` is for: `udev_device_get_is_initialized()`. libinput skips any
`event*` node for which it is false, and weston then dies with

```
warning: no input devices on entering Weston. Possible causes:
	- no permissions to read /dev/input/event*
	- seats misconfigured (Weston backend option 'seat', udev device property ID_SEAT)
```

Both of those suggested causes are wrong. The nodes were present and readable;
they were simply never marked configured. libinput's own log gives it away at
`INFO` level:

```
event3  - skip unconfigured input device '/dev/input/event3'
```

Input entries are generated from `/sys/class/input` alongside the DRM ones,
classified by device name and by whether the node reports absolute axes. That
is enough to separate `fts_ts`, the touchscreen, from the keys and the headset
jack without pulling in udev's hwdb.

## The renderer is pixman, and it has to be

The obvious choice is GL on llvmpipe, and it initialises fine on the headless
backend. On the DRM backend it cannot work, for two independent reasons.

Mesa's DRM path goes through GBM, where the only usable driver is
`kms_swrast`, which requires `DRI_DRI2`. SDE does not implement that
extension, so every DRI driver fails to bind:

```
gbm: did not find extension DRI_DRI2 version 1
failed to bind extensions
failed to load driver: swrast
```

And the glvnd dispatch layer never loads a vendor here - `/usr/share/glvnd/
egl_vendor.d/50_mesa.json` is present and correct, but the result is

```
warning: EGL_EXT_platform_base not supported
failed to create display
```

Bypassing glvnd with `LD_LIBRARY_PATH` pointing at mesa's own EGL does get
further, then dies on `undefined symbol: eglTerminate`, because weston has
already loaded the glvnd one.

pixman needs no GPU and no EGL, so it sidesteps all of it. Slow, and that is
fine - the point of this stage is that the panel, the input and the Wayland
socket all work, none of which depends on the renderer being fast.

Renderer and device go in `weston.ini`, not on the command line. weston passes
its whole argv to the shell module, and both `desktop-shell` and `kiosk-shell`
abort on an option they do not recognise - including `--renderer`, which is
precisely the option that has to be set.

## weston 10 aborts on a duplicated format

```
weston: ../libweston/drm-formats.c:131: weston_drm_format_array_add_format:
Assertion `!weston_drm_format_array_find_format(formats, format)' failed.
```

An upstream bug. `drm_plane_populate_formats()` in `libweston/backend-drm/
kms.c` walks a plane's `IN_FORMATS` modifier blob and adds each entry with no
duplicate check, and SDE advertises the same fourcc twice.
`weston_drm_format_array_join()`, a few lines below, does check - so the
invariant is real, just not enforced consistently.

`tools/fmtshim.so` intercepts `weston_drm_format_array_add_format` and
`weston_drm_format_add_modifier` and returns the existing entry instead of
asserting. Both are needed; with only the first, the second assertion fires
immediately. The duplicate carries a different modifier, so returning the
existing entry is not the same as discarding data.

Interception rather than a patch because the call crosses a shared-object
boundary from `drm-backend.so`, which makes it interposable, and
`libweston-10.so.0` is a Debian binary package.

## What is not solved

GPU. KGSL exposes no DRM node, so neither Mesa nor freedreno can see the
Adreno 619, and `msm_drm_dri.so` does not exist. Every GL path on this device
ends in software or in nothing.

`weston-desktop-shell` and `weston-keyboard` are installed but need more
X11 and pango libraries than are present; `kiosk-shell` is used instead, which
is the right shell for a full-screen phone display anyway.

The device reboots repeatedly during display work, which takes the USB link
with it and destroys all runtime state. `lindroid` rebuilds that state on every
run, so it is safe to just run it again.
