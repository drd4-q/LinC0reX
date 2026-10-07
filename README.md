# LinC0reX

Ordinary Linux on a Xiaomi phone, with the physical display working.

postmarketOS boots on the Poco X5 5G / Redmi Note 12 5G (codename
`moonstone`), the panel is driven by an out-of-tree DRM driver, and the
touchscreen works. Stock Android is never broken: it boots, it comes back,
and the display is handed over cleanly in both directions.

Everything is built from source by one script and shipped as a single
recovery zip.

```
git clone https://github.com/drd4-q/LinC0reX
cd LinC0reX
./install.sh
```

Full overview: [docs/overview.md](docs/overview.md)

## Status

| | |
|---|---|
| Boot | postmarketOS with OpenRC, `switch_root` reached |
| Display | 1080x2400 through `drm_fbdev_emulation` and fbcon |
| Touch | Focaltech `fts_ts`, `/dev/input` populated by the kernel |
| Access | root over USB serial, no password, survives reboot |
| GPU | not reachable from userspace, software rendering only |

## What this is built on

Nothing here is written from nothing. Three things went in:

- **Kernel base** — [kamikaonashi/kernel_xiaomi_stone](https://github.com/kamikaonashi/kernel_xiaomi_stone)
  branch 16, the Darkmoon-Reborn tree. This is the base we build on and the
  only kernel this phone actually runs. The display stack, the SDE
  (Qualcomm Display Engine) panel driver and the device tree all come from
  it.

- **Lindroid** — an earlier project that got Linux running on Android
  phones, with a `lindroid-drm` display driver derived from DisplayLink's
  `evdi`, which it renamed and extended. **That project is not ours.** We
  took its driver and a number of its ideas, and the identifiers inside
  the driver still carry its name: `drivers/lindroid-drm/`,
  `CONFIG_DRM_LINDROID_EVDI`, `evdi_lindroid_drv.c`. They are left as they
  are on purpose — renaming them would make the provenance harder to see,
  not easier. (Upstream URL to be filled in once confirmed; the driver
  itself records DisplayLink's copyright, see below.)

- **postmarketOS** — pmbootstrap builds the userland. Its initramfs scripts
  are MIT and we patch them in place; the patches live in
  [tools/pmos-pmos-initramfs.sh](tools/pmos-pmos-initramfs.sh).

Licensing, including why the DRM driver has to be GPL-2.0-only and not
GPL-3.0, is in [docs/licensing.md](docs/licensing.md).

## Authorship

**This project was written by an AI coding agent** — OpenCode — working
under the direction of the repository owner, who chose the hardware, made
every call about risk, tested everything on a real phone, and is
answerable for what is committed here.

It is stated plainly because the code is going up as public work and the
provenance is part of it. Nothing in this repository was written by a human
typing at a keyboard.

The errors are recorded, not quietly amended. Several commits document a
wrong conclusion, what the evidence actually showed, and what it cost to
reach the next answer.

## Flashing

```
adb sideload out/pmos-console-xiaomi-moonstone-YYYYMMDD.zip
```

From TWRP or OrangeRecovery. The installer repartitions `userdata`, so
everything on it is lost. Stock Android is untouched and restorable with:

```
fastboot flash boot ~/kernel_xiaomi_stone/stock/boot.img
```

## Documentation

**Getting started**

- [Install and flash](docs/pmos-flash.md) — the whole procedure
- [Runbook](docs/pmos-runbook.md) — command by command
- [Status](docs/pmos-status.md) — where things stand

**What went wrong, and why it is written down**

- [Boot failure](docs/pmos-boot-failure.md) — the header version trap
- [Device tree](docs/pmos-dtb.md) — where the tree comes from
- [Page flip root cause](docs/page-flip-root-cause.md) — a kernel patch
  and a flash spent on a wrong diagnosis
- [vblank](docs/vblank.md)

**Display**

- [Panel handover](docs/panel-handover.md) — Android/Linux switching
- [Display control](docs/display-control.md)
- [Home screen](docs/home-screen.md) and [xdg-popup](docs/xdg-popup.md)
- [GPU path](docs/gpu-path.md) — why there is no acceleration
- [Weston on the phone](docs/weston-on-phone.md)

**Project**

- [Licensing](docs/licensing.md)
- [Overview](docs/overview.md) — the long version

## License

GPL-2.0-only. See [LICENSE](LICENSE) and
[docs/licensing.md](docs/licensing.md).
