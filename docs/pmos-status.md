# Current state, 2026-10-05, late evening

Written because the computer was about to be switched off mid-investigation.
Everything below is measured, not planned.

## Where the phone is

Booted with `out/boot-diag3.img`: our kernel, stock v3 header, **real pmOS
initramfs** with three patches plus diagnostics. It reaches pmOS's **debug
shell over USB**, with a root prompt:

```
postmarketOS debug shell
  Device: Xiaomi POCO X5 5G / Redmi Note 12 5G (xiaomi-moonstone)
  Kernel: 5.4.302-Darkmoon-Reborn
  initrd: 3.12.3-r1
Run 'pmos_continue_boot' to continue booting.
Read the initramfs log with 'cat /pmOS_init.log'.
~ #
```

Read it with `python3 tools/lind-console.py 150`, and send commands with
`python3 /tmp/opencode/send.py '<команда>' 60` (recreate that file if /tmp was
cleared - it is just: open `/dev/ttyACM0` `O_RDWR`, write `cmd\n`, then read).

## What is installed and verified

The pmOS install completed. From `/tmp/postmarketos/pmos.log`, pulled with
`adb pull`:

```
+ findfs 'PARTLABEL=userdata'   -> dev=/dev/block/sda19
+ findfs 'PARTLABEL=boot_a'     -> dev=/dev/block/sde9
+ dd 'if=/mnt/pmOS/boot/boot.img' 'of=/dev/block/sde9'
134217728 bytes (128.0MB) copied
"install completed with status 0"
```

Read back off the phone and checked by md5:

- `boot_a` == `out/boot-pmos-final.img`, md5 `5c56d10e02502bdfadd9b4de2edd93ca`
- `userdata` has `pmOS_boot` (236 MiB) and `pmOS_root` (222 GiB), real ext
  filesystems, labels intact

**The partition table had to be corrected by hand.** `parted` could not inform
the kernel - `Error: Partition(s) 1..64 ... unable to inform the kernel of the
change, probably because it/they are in use` on all four commands - so `kpartx`
used a stale geometry and the filesystems were created at 8 MiB and 244 MiB
while the table said 1 MiB and 30.5 MiB. `mkfs` also sized `pmOS_root` to
58313472 blocks of 4 KiB (222 GiB) while `parted` had recorded the same number
of *sectors* (27.8 GiB).

The fix was one 512-byte sector, written from recovery with `dd`:

```
p1: type=0x83 boot=0x80 start=16384  секторов=483328      -> 8.00..244.00 МиБ
p2: type=0x83 boot=0x00 start=499712 секторов=466509464  -> 244.00 МиБ .. конец
```

`p1` ends exactly where `p2` begins, and both match the filesystem positions.
Verified byte-for-byte after writing. Device size is 467009176 sectors.

## Why pmOS falls into the debug shell

From `cat /pmOS_init.log`:

```
  ❬❬ PMOS STAGE 1 ❭❭
  ❬❬ PMOS STAGE 2 ❭❭
  Setting up USB gadget through configfs
Trying to start server with parameters: 172.16.42.1:67, client 172.16.42.2, interface: usb0
Trying to mount subpartitions for 10 seconds...
ERROR: failed to mount subpartitions!
Entering debug shell
```

**`fdisk` is not in the initramfs.** `mount_subpartitions()` in
`init_functions.sh` counts subpartitions with it:

```sh
part_count="$(fdisk -l "$partition" 2>/dev/null | grep -cE '^ +[0-9]|^'"$partition")"
```

Without `fdisk` the count is always 0, so it never reaches `losetup` and reports
failure. Present in the initramfs: `wget`, `udhcpc`, `losetup`, `blkid`,
`parted`, `switch_root`, `dmsetup`, `e2fsck`. Absent: `tar`, `mkfs.*`,
`fdisk`.

Two ways to fix, either is fine:

- patch `mount_subpartitions` to count with `/proc/partitions` or
  `/sys/block/<dev>/<part>` instead of `fdisk`
- or add `fdisk` from util-linux into the initramfs

Also seen, and not fatal: `modprobe: FATAL: Module vfat not found in directory
/lib/modules/5.4.302-Darkmoon-Reborn` - the rootfs carries mainline 7.2.0
modules, useless for our kernel. `/dev/fb0 is not available` is expected, there
is no framebuffer console in this kernel.

## The exact point where it stopped

From the debug shell, manually:

```
~ # losetup -a
/dev/loop0: [0002]:15871 (/tmp/logs.img)      <- pmOS's own log disk holds loop0

~ # losetup -P /dev/loop0 /dev/sda19
losetup: /dev/sda19: failed to set up loop device: Resource busy

~ # losetup -P /dev/loop1 /dev/sda19
rc=0                                             <- attached successfully
~ # ls /dev/loop*p*
ls: /dev/loop*p*: No such file or directory       <- no device NODES
~ # blkid | grep pmOS
(nothing)
```

So the attach works and the table is parsed, but the nodes do not appear -
`CONFIG_DEVTMPFS` is unset and nothing runs mdev after the attach. That is the
same wall as before, and it is one `mdev -s` plus `mknod` away.

## What to run next, in the debug shell

```sh
mdev -s
ls /dev/loop*p*
grep loop /proc/partitions
```

If the nodes are still missing, create them by hand - major 7 for loop, minors
from `/proc/partitions`:

```sh
grep loop /proc/partitions
mknod /dev/loop1p1 b 7 <minor1>
mknod /dev/loop1p2 b 7 <minor2>
blkid | grep pmOS
```

Then mount and switch:

```sh
ROOT=$(blkid | grep pmOS_root | cut -d: -f1)
BOOT=$(blkid | grep pmOS_boot | cut -d: -f1)
mkdir -p /sysroot/boot
mount -t ext4 -o rw "$ROOT" /sysroot
mount -t ext2 -o rw "$BOOT" /sysroot/boot
ls /sysroot | head
ls /sysroot/boot
```

And if that all works, `pmos_continue_boot`.

Expect `pmOS_root` to appear now - the MBR matches the filesystems, and that was
the one thing that had never been true before.

## To bake the fix into the image

`tools/pmos-pmos-initramfs.sh` is the tool, and `tools/pmos-lind-console.py`
reads the port. Add the `mdev -s` + `mknod` for loop partitions into the
`lind_loopnodes` function (it is already there) **and** make `lind_loopnodes`
run *before* anything that depends on the root, because `mount_subpartitions`
gives up after 10 seconds and takes the boot to the debug shell with it.

The cleanest version: replace the `fdisk` call in `mount_subpartitions` with
`/proc/partitions`, then pmOS's own code path works and none of this is needed.

## Traps on this device, all measured

- **`fastboot flash boot_a` does not stick.** It reported `OKAY` four times and
  the partition was unchanged. Write boot images with `dd` from recovery.
- **`adb exec-out dd if=/dev/block/...` returns wrong data.** Two reads of the
  same partition disagreed. Build the file on the phone, `adb pull`, pull twice.
- **A boot image built by `mkbootimg` will not boot.** `header_version=0`,
  `page_size=4096`; this bootloader takes `header_version=3`, `page_size=0`,
  empty cmdline. Always `magiskboot unpack` the **stock** boot.img and repack.
- **There is no console.** `CONFIG_FRAMEBUFFER_CONSOLE`, `CONFIG_VT`,
  `CONFIG_DRM_FBDEV_EMULATION` all unset, and the cmdline is empty, so
  `/dev/console` goes nowhere. `/dev/ttyGS0` is the only output.
- **`exec >/dev/ttyGS0` kills the phone**, reboot about three seconds after the
  gadget comes up. Emit line by line with a pause; 12 kB that way is fine.
- **No udev.** `# CONFIG_DEVTMPFS is not set`, `CONFIG_UEVENT_FS` unset. Every
  `/dev/block/by-name` and `/dev/disk/by-partlabel` symlink is created by hand
  in `lind_devsymlinks`.
- **The kernel carries our panel TE patch nowhere.** `te-pin-select` is absent
  from the m17 panel nodes in the live tree; vblank works anyway. See
  `docs/pmos-dtb.md`.
- Recovery lives inside `boot_a`, so replacing it removes recovery and
  `fastboot reboot recovery` just falls back to fastboot.
- `fastboot` has no `poweroff`. Use `python3 tools/lind-console.py --send off`
  from our init, or hold Power for ten seconds.