# Lindroid

Running ordinary Linux on a Xiaomi phone, and letting it have the physical
display — without bricking Android in the process.

The phone stays a phone. Android boots, Android comes back, and the display
hands over cleanly in both directions. What Linux gets is a Debian chroot, a
compositor of its own, and control of the panel.

**Target:** Poco X5 5G / Redmi Note 12 5G, codename `sunstone`.

---

## Author and attribution

**This project was written by an AI coding agent** — OpenCode, running the
`space-bunny-free` model — working under the direction of the repository owner,
who chose the hardware, made every call about risk, tested everything on a real
phone, and is answerable for what is committed here.

That is stated plainly because the code is going up as public work and the
provenance is part of it. Nothing in this repository was written by a human
typing at a keyboard, and no commit here should be read as if it were.

The owner has been explicit about one thing that matters for a public repo:
**the errors are recorded, not quietly amended.** Several commits here document
a wrong conclusion, what the evidence actually showed, and what it cost to reach
the next answer. That was requested and it is deliberate — see
[docs/page-flip-root-cause.md](docs/page-flip-root-cause.md) for the sharpest
example, where a whole kernel patch and a flash were spent on a fault that was
never there.

---

## What this is

- A **display-control driver** (`evdi-lindroid`) giving a Linux process DRM master
  on the panel, so it can drive the screen directly.
- **A compositor of its own** (`lind`), CPU-rendered, speaking enough Wayland for
  real clients: `wl_compositor`, `wl_shm`, `wl_subcompositor`, `wl_output`,
  `wl_seat`, and `xdg_wm_base` with toplevels.
- **A desktop** drawn by that compositor, with an app grid whose geometry is
  taken from Phosh.
- **A handover procedure** that takes the panel from Android and gives it back,
  and that was learned the hard way.

## What this is not

Not a phone OS, not a replacement for Android, and not finished. It boots a
Linux userland on hardware that has no GPU path exposed to it, with software
rendering, and it does the display work only. See
[Known limitations](#known-limitations) — they are not small.

---

## Quick start

```sh
./setup.sh                       # clone the kernel base, KernelSU-Next, apply patches
cd ~/dm-kernel && ./build.sh     # build and package a flashable zip

# on the phone
/data/lindroid/bin/lindroid status    # who holds the display
/data/lindroid/bin/lindroid home      # the desktop, no client
/data/lindroid/bin/lindroid demo 40   # desktop + a client drawing
/data/lindroid/bin/lindroid back      # give the panel back to Android
```

`home` and `back` are the two worth memorising. `back` is the one that matters
if anything goes wrong.

---

## How the display handover works

Two Android services hold file descriptors on `/dev/dri/card0` and will not give
them up:

| service | why it matters |
| --- | --- |
| `surfaceflinger` | owns the framebuffer |
| `vendor.qti.hardware.display.composer` | the display HAL, holding 6 fds on card0 *independently* of SurfaceFlinger |

Stopping only SurfaceFlinger is not enough, and the symptom is misleading: the
SetCrtc succeeds and the screen shows nothing useful. Both must stop.

On the way back, **the composer starts before SurfaceFlinger**. The other order
leaves Android without a display until something else restarts it.

Ownership is checked by looking for holders of `card0`, not by assuming — see
`lindroid status`.

---

## Display-control ioctls

The driver adds three ioctls to the EVDI virtual display, at command numbers
`0x10`–`0x12`, immediately after the existing EVDI commands. Nothing already
shipped is touched.

| ioctl | does |
| --- | --- |
| `DRM_IOCTL_EVDI_CONTROL` | get/set state |
| `DRM_IOCTL_EVDI_GET_INFO` | reports one display in full |
| `DRM_IOCTL_EVDI_WAIT_EVENT` | blocks up to 5 s on a change |

Full interface: [docs/display-control.md](docs/display-control.md).

---

## The compositor

`tools/lind.c`, one program, ~1800 lines, CPU only.

**Presentation** is a double-buffered page flip. Getting there took the longest
of anything in this project, and the reason is worth stating because it was not
obvious: the compositor was matching the flip completion against
`DRM_EVENT_PAGE_FLIP_DONE = 0x04`, while this kernel sends
`DRM_EVENT_FLIP_COMPLETE = 0x02`. The completion *was* arriving and being thrown
away, so the compositor never requested a second flip and presented one frame —
which is indistinguishable, from the outside, from a driver that never completes
a flip. `frames=1` and a silent driver look exactly the same.

**Input** works end to end, and each of the four bugs in it presented
identically as "the gesture does nothing":

1. `fts_ts` reports multi-touch protocol B only — `ABS_MT_POSITION_X/Y`, with no
   `ABS_X`/`ABS_Y` at all.
2. It reports 10800×24000 for a 1080×2400 panel. The range is read from the
   device rather than assumed, since that ratio belongs to this digitiser.
3. The touch state was local to the read callback and reset every wakeup, so
   every batch after the first looked like the finger being lifted.
4. `wl_pointer_send_motion` takes `wl_fixed_t` — 24.8 fixed point. Passing
   display pixels as integers divided every coordinate by 256: (540,1200)
   arrived as (2,4).

**Brightness** is a sysfs backlight that SurfaceFlinger normally owns, so taking
the display leaves the panel wherever it last was — 315 of 2047 on this phone,
which reads as a dim screen rather than as a fault. `lind` sets it to full and
follows a vertical drag, writing at 20 Hz with a step limit, because a write per
touch sample stalled the compositor and unbounded steps read as flicker.

---

## The desktop

Drawn by the compositor into the same buffer as client content. The renderer is
not modified.

The grid geometry is Phosh's, taken from `phosh_0.24.0/src/ui/app-grid-button.ui`,
which states it outright:

    (360px screen width - 2*3px flowbox margins - (4-1)*6px column spacing)
    / 4 columns = 84px

Four columns, 3 dp margin, 6 dp spacing, 84 dp button, on a 360 dp screen. This
panel is 1080 px wide — 360 dp at a density of exactly 3 — so every value is
`dp * 3` and none of it is guessed.

What is ported is the arithmetic and the interaction model. The QML is not, and
cannot be: it is executed by Qt Quick at runtime, and Qt Quick needs a GPU buffer
path (`wl_drm` or `linux-dmabuf`, `wp_viewporter`, `xdg_decoration`, EGL) that
`lind` does not implement. Menus do work — `xdg_popup` is implemented, including
`xdg_positioner` and the on-screen constraint that a menu hanging off the top
edge would otherwise never be seen; see [`docs/xdg-popup.md`](docs/xdg-popup.md)
for what that gap actually was.

Apps come from `/usr/share/lindroid/apps`, one per line:

    Name | command | tint

Note on KDE Plasma Mobile: `plasma-mobile` **is** packaged for bookworm arm64
(5.27.2, the Qt5 generation) and its direct dependencies all resolve. Running it
here is not a packaging problem but a protocol one — see above.

Details: [docs/home-screen.md](docs/home-screen.md).

---

## The kernel

### This repository does not contain the kernel source

The kernel is a separate tree, and it stays a separate tree:

    https://github.com/kamikaonashi/kernel_xiaomi_stone   (branch 16)

`uname -r` on the shipped ROM is `5.4.302-Darkmoon-Reborn`, which is what this
tree builds — the identity is matched deliberately so nothing keys off the
string.

Copying the tree into this repository would duplicate another project's GPL-2.0
work and blur its provenance. **The correct way to publish a kernel change is as
a patch against that tree**, which is exactly what exists:

| where | what |
| --- | --- |
| this repo, `patches/0001-lindroid-drm.patch` | the display-control driver |
| this repo, `patches/0002-ksu-namespace-support.patch` | KernelSU-Next on 5.4 |
| kernel tree, branch `fix/page-flip-completion` | flip-completion fix |

So: the kernel work is published as patches plus one branch, not as a copy.

### Why Darkmoon-Reborn and not the Protium fork

Both sit on 5.4.302. Protium boots, but Android then comes up degraded — wrong
UI, no lock screen, no wallpapers, as if freshly set up. Verified by building
Protium with and without the changes. The shipped kernel is built with clang 21;
newer clang flags implicit enum-to-enum conversion as an error and the out-of-tree
WiFi driver has plenty, hence `-Wno-implicit-enum-enum-cast`.

### The flip-completion patch — unfinished

Branch `fix/page-flip-completion`, commit `32770983e`, in the kernel tree.
Compiles clean. **It is not finished and its necessity is unproven.**

What is sound: it fixes two real faults in the driver's event-caching path. It
reads `crtc->state->event` — the *old* state — while `page_flip_common()` sets
the event on the new one; and the commit that caches it sits behind
`drm_atomic_crtc_needs_modeset()`, which is false for a page flip, so it is never
reached.

What is not: whether any of it was needed. The one build that could have shown
the driver "never sends" the completion was already being corrected in userspace
at the same time, so nothing separates the two. It also completes the flip when
the commit lands rather than at the vblank, which leaves a tear — see
[Known limitations](#known-limitations).

**The next thing to do with the kernel** is to find where SDE delivers vblank
events. The tree contains no call to `drm_vblank_handler()` anywhere outside
`drm_vblank.c`, yet the compositor receives `DRM_EVENT_CRTC_SEQUENCE` with a
usable rate, so the path is not the obvious one. Completing the flip from that
interrupt instead of from the commit path is what would let the compositor's own
UI stop tearing.

### Building and flashing

```sh
./setup.sh
cd ~/dm-kernel
./build.sh                 # → Darkmoon-<date>.zip, AnyKernel3, boot only
```

The zip ships the kernel alone — **no `dtb`/`dtbo`**. The stock `boot.img`
carries no appended DTB and `ak3-core.sh`'s `flash_boot` silently appends any it
finds in the zip root, which changes how the panel initialises. There is also a
regression test for the other trap: `EVDI_DEFAULT_NR_DEVICES = 0`, because a DRM
card created at init claims `/dev/dri/card0` and pushes the real panel to
`card1`, and Android hardcodes `card0`.

Rollback is the stock `boot.img` in `stock/`.

---

## Repository layout

```
build.sh                 build and package the kernel
setup.sh                 clone the base tree, KernelSU-Next, apply patches
patches/                 changes against the kernel tree
drivers/lindroid-drm/    the display-control driver (fork of DisplayLink EVDI)
tools/lind.c             the compositor
tools/lindroid           the handover script
tools/lindtest.c         a client
tools/*.c                probes and measurements, kept because they were useful
docs/                    findings, including the wrong ones
```

---

## Known limitations

These are real, and none of them are "coming soon" in any promised timeframe.

**There is a tear, and UI makes it visible.** `PAGE_FLIP_DONE` arrives when the
commit lands rather than at the vblank, so the buffer swap is immediate and there
is always a tear. It was invisible on a full-screen gradient — a tear in a
gradient looks like the gradient — and became obvious the moment a hard-edged bar
was drawn across the top. Bisected with `LIND_NOPANEL`: without the top bar the
animation does not flicker; with it, it does. The fix is one line of driver work.

**No GPU.** KGSL exposes no DRM node, so Mesa and freedreno cannot see the
Adreno 619. Everything is software rendered, and a full frame must fit in one
refresh period or it tears — measured at 31 fps against a 120 Hz panel for a
banded renderer that could not be made to fit.

**Not a usable phone shell.** Menus work, so `xdg_popup` is no longer the
blocker — `xdg_decoration` and `wp_viewporter` are, and every surface is
fullscreen. `phosh` will not run. `weston-terminal` needs more X11 and pango
libraries than the chroot has.

**Android's input may not come back by itself.** Stopping SurfaceFlinger takes
the input pipeline with it, and it does not always recover. The device is not
held by anything of ours — `system_server` holds it, which was verified — so this
is Android's pipeline, not ours. `lindroid reboot` is provided; a reboot is the
reliable fix.

---

## Documentation

| | |
| --- | --- |
| [docs/panel-handover.md](docs/panel-handover.md) | taking and giving back the display |
| [docs/display-control.md](docs/display-control.md) | the ioctl interface |
| [docs/weston-on-phone.md](docs/weston-on-phone.md) | what weston does on this hardware |
| [docs/home-screen.md](docs/home-screen.md) | the desktop, and Phosh's geometry |
| [docs/xdg-popup.md](docs/xdg-popup.md) | menus, and why a missing line in a table read as "popups don't work" |
| [docs/page-flip-root-cause.md](docs/page-flip-root-cause.md) | **read this one** — the flip investigation, including the conclusion that was wrong |

---

## Licences and third-party code

This repository is **GPL-2.0-only**. The full text is in [LICENSE](LICENSE),
copied verbatim from the kernel tree.

- `drivers/lindroid-drm/` is a fork of **DisplayLink's EVDI**, which is
  GPL-2.0. DisplayLink's copyright notices are preserved in the sources and in
  `dkms.conf` alongside ours. This is a derivative work and is distributed under
  the same licence.
- `patches/` are patches against a third-party kernel tree, GPL-2.0.
- `tools/` is original work, GPL-2.0-only to match the repository.
- **KernelSU-Next** is fetched by `setup.sh` and is licensed by its authors.
- **weston**, **Debian packages** and everything else in the chroot carry their
  own licences and are not part of this repository.

The kernel tree itself — `kamikaonashi/kernel_xiaomi_stone` — is a separate
GPL-2.0 project by its authors and is not vendored here.