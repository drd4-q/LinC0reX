# Why the pmOS image would not boot, and what fixed it

Measured 2026-10-04. This is the long version of "the phone kept going to
fastboot and we could not see why".

## The blind spot

Everything below was found without ever seeing kernel output, because there is
no way to see it on this device without help:

- `CONFIG_FRAMEBUFFER_CONSOLE`, `CONFIG_VT` and `CONFIG_DRM_FBDEV_EMULATION` are
  not set in `out/.config`. There is no `tty0`. `console=tty0` is not a choice
  that could be made differently, the driver does not exist.
- `CONFIG_CMDLINE="cgroup_disable=pressure"` with `CONFIG_CMDLINE_EXTEND=y`. The
  stock boot image has an **empty** cmdline, so the real command line is
  `cgroup_disable=pressure` plus what the bootloader appends. Reading
  `/proc/cmdline` on Android gives `msm_rtb.filter=0x237
  service_locator.enable=1 androidboot.dtb_idx=0 rootwait init=/init` and so on,
  and **no `console=` anywhere**. Android has no kernel console either.
- The only other console is `ttyGS0`, and it does not exist until userspace
  creates the ACM gadget function.

So a failure before that point is unobservable, which is exactly what happened.

## What was ruled out, with evidence

**AVB.** `vbmeta_a` is 64 KiB of zeros with the magic `AVB0` and every header
field zero - `algorithm_type = 0`, i.e. `AVB_ALGORITHM_NONE`, no descriptors.
Verification is off. This was the first hypothesis and it was wrong.

**The device tree.** The bootloader passes 393908 bytes from `vendor_boot_a` plus
an overlay from `dtbo_a`; see `pmos-dtb.md`. Nothing to fix.

**The install itself.** The pmOS installer did its job: `pmOS_boot` (236 MiB)
and `pmOS_root` (222 GiB) exist inside `userdata` with correct ext superblocks.
It never wrote `boot.img`, because `extract_partition_table()` returns 1 when
`findfs PARTLABEL=boot_a` fails and `pmos_install` has no `set -e`, so it carried
on and `dd`-ed into an empty variable. Confirmed by reading `boot_a` back: it
was byte-identical to `stock/boot.img`.

**`fastboot flash`.** Reported `OKAY` four times and the content never changed.
Writing the same bytes with `dd` from recovery worked and was verified by md5.
For this device, write boot partitions with `dd`, not `fastboot flash`.

**A measurement trap worth recording.** `adb exec-out dd if=/dev/block/...`
returns plausible data and it is wrong: two reads of the same partition differed.
Every conclusion drawn from it, including "the image is corrupted" and "the
kernel size is nonsense", was an artefact. The reliable pattern is to build the
file on the phone and `adb pull` it; two pulls then match. Also `/data/local/tmp`
does not exist after `/data` is wiped - use `/tmp`.

## The actual cause

Compare the header of the two images that work with the one that would not:

| field | `boot_a` stock | `boot_b` (slot-successful) | pmOS image |
|---|---|---|---|
| `header_version` | 3 | 3 | **0** |
| `page_size` | 0 | 0 | 4096 |
| `os_version` | 0 | 0 | 0 |
| cmdline | empty | empty | ours |

Both working images are `magiskboot repack` of the stock boot image, so their
header is the stock header copied verbatim, including its non-conforming
`page_size` of 0. The pmOS image was built from scratch by `mkbootimg`, which
produced a spec-correct header - and this bootloader will not boot it.

`deviceinfo_bootimg_header_version="3"` and `deviceinfo_bootimg_os_version` never
reached `mkbootimg`, which is why the os_version and patch level we invented
were a red herring.

## The fix

Build the boot image from the **stock** boot image, not from the pmOS one:

```sh
magiskboot unpack -h stock/boot.img     # header comes from here
cp our-Image kernel
cp our-initramfs ramdisk.cpio
printf 'cmdline=\nos_version=16.0.0\nos_patch_level=2026-04\n' > header
magiskboot repack stock/boot.img out.img
```

With that, the phone stopped going to fastboot. `/dev/ttyACM0` and
`/dev/ttyACM1` appeared 15 seconds after reboot, which proves the kernel booted,
the initramfs ran and the gadget was configured.

Note that magiskboot writes `cmdline` four bytes low when the base header is
v3 - it lands on `os_version`. So with an empty cmdline there is no
`console=` at all, and `/dev/console` goes nowhere. Output has to be written to
`/dev/ttyGS0` directly; the kernel console is not needed for that.

## Also learned about the recovery path

Recovery lives inside `boot_a` on this device. Replacing `boot_a` removes it,
and `fastboot reboot recovery` then just falls back to fastboot. Restoring a
boot image that contains recovery is what gets the phone back.

## What still needs doing

The phone boots to the logo and hangs. No output has ever been read from
`/dev/ttyGS0`, so nothing past "the kernel starts" is confirmed. The next image
to flash is the one from `tools/pmos-debug-initramfs.sh` whose `/init`
redirects into `/dev/ttyGS0` with `exec >/dev/ttyGS0 2>&1` right after creating
the gadget, instead of relying on `/dev/console`.