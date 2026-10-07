# Flashing pmOS on this phone, and getting a console back

Written 2026-10-04. This is the procedure that produced
`out/pmos-lind.zip`, md5 `514ca2f84ffc40553fcb5e5316b00401`.

## What the zip is

`adb sideload` it from AOSP recovery. It is a pmOS recovery flashable that
formats the rootfs and boot partitions, so **`/data` is erased by the install
itself**, not just by any earlier factory reset.

Before flashing, put the backup back where it can be reached *after* the wipe:

```sh
# after pmOS is up, over the USB network link - see below
```

`~/lindroid-backup.tar` is 3.0 GB and is the only copy of `droot/`, `pmos/`,
`alpine/` and `bin/`. Nothing on the phone is a substitute for it.

## Getting a console afterwards

There is no adb, no network driver and no framebuffer console under pmOS: the
rootfs carries modules for mainline 7.2.0, and the kernel is our 5.4.302, so no
modules load. The panel needs SDE, so `console=tty0` has no framebuffer behind
it either. Without help the phone boots to a black screen and stays there.

The help is the USB port, and almost all of it already existed.

`init_functions.sh` in the pmOS initramfs mounts configfs and builds a USB
gadget on its own, in `setup_usb_network_configfs()` called from
`setup_usb_network()`:

- gadget `g1`, `idVendor 0x18D1`, `idProduct 0xD001`
- one network function, `ncm.usb0`, falling back to `rndis.usb0`
- UDC bind, then `unudhcpd` offers `172.16.42.2`

What it did not build is a serial function. So `tools/pmos-usb-console.sh` adds
one before the UDC bind, which is the last moment at which the configuration
can still be changed:

```sh
mkdir "$CONFIGFS/g1/functions/acm.GS0"
ln -s "$CONFIGFS/g1/functions/acm.GS0" "$CONFIGFS/g1/configs/c.1/acm.usb"
```

`CONFIG_USB_CONFIGFS_ACM=y` is in our `out/.config`, so no module is needed. The
command line gets `console=ttyGS0,115200n8` to go with it.

Note that `run_hooks()` is defined in that same `init_functions.sh` and
**nothing calls it**, and there is no `/hooks` directory. A hook dropped there
would never run. Hence the edit to the function that already runs.

### Reading it

```sh
ls /dev/ttyACM*          # appears once the phone has booted
screen /dev/ttyACM0 115200
# or: picocom -b 115200 /dev/ttyACM0
```

The port registers late, by design: the kernel parses `console=ttyGS0` before
the initramfs has created the function, and console output resumes when it
appears. Everything before that point is lost, which is the same as having no
console at all.

### Moving files

Two ways, once the console is up:

- **network**: the gadget already offers `172.16.42.2` over NCM/RNDIS, and the
  PC gets a DHCP lease from the phone. `nc`, or busybox `httpd` on the phone,
  is enough. Check `ip addr` on both ends before assuming it failed - NCM needs
  the host side to have `cdc_ncm` or `ncm` module.
- **USB mass storage**: add `mass_storage.0` to the same config from the
  running system. Unlike the initramfs, the running system has a real
  filesystem to back it with, which matters for a 3 GB archive.

## Where the device tree comes from

Measured, and it is not a problem: the bootloader passes it. The base tree lives
in `vendor_boot_a` (`/dev/block/sde18`) and an overlay in `dtbo_a`
(`/dev/block/sde13`), both physical partitions of the same UFS LUN as `boot`.
Not in `super`, not in `/data`, so a factory reset does not touch them.

This image is deliberately shaped to rely on that: kernel plus initramfs, no
`dtb` slot, no appended tree. See `docs/pmos-dtb.md` for the measurements,
including the finding that our panel TE patch is not actually on the phone.

Also worth knowing before flashing: under pmOS there will be no kernel modules,
because the rootfs carries them for mainline 7.2.0 while the kernel is our
5.4.302. So no Wi-Fi, no touchscreen, and no `apk add` of anything that needs a
module until that is sorted out.