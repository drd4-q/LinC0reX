# The installer writes a partition table that is 8x too small

## What happens

`adb sideload` of the recovery zip completes, the phone reboots, and pmOS
loads its initramfs. Then:

```
Mount subpartitions of /dev/sda19
Mount subpartitions of /dev/sda19
ERROR: failed to mount subpartitions!
Entering debug shell
```

`mount_subpartitions` never finds `pmOS_root`, so init drops to the debug
shell instead of switching root. Nothing about the zip is wrong, and nothing
about the kernel is wrong: the root filesystem is created correctly, it is
simply at a different address than the partition table says.

## Diagnosis

From the debug shell on the phone:

```sh
fdisk -l /dev/sda19
#   /dev/sda19p1 *     2048    62463    60416  1.8G 83 Linux
#   /dev/sda19p2      62464 58375935 58313472  222.4G 83 Linux
```

`fdisk` reports the device as `Units: sectors of 1 * 4096 = 4096 bytes`, so
those numbers look plausible. They are not. The msdos partition table stores
LBAs in **512-byte units**, always, and nothing in the format cares what the
device's logical sector size is.

Multiply by the 512-byte unit and the table points at:

```
p1: 2048  * 512 =     1 MiB
p2: 62464 * 512 =  30.5 MiB
```

The filesystems are not there. Check for the ext2/3/4 superblock magic
`0xEF53`, which lives at offset 1080 of the filesystem:

```sh
# the real pmOS_boot
dd if=/dev/sda19 bs=1 skip=8389688 count=2 | od -An -tx1   # 8 MiB + 1080
#   53 ef
# where the table says p2 is
dd if=/dev/sda19 bs=1 skip=31982648 count=2 | od -An -tx1  # 30.5 MiB + 1080
#   1a f8
```

And the root filesystem:

```sh
dd if=/dev/sda19 bs=1 skip=255853624 count=2 | od -An -tx1 # 244 MiB + 1080
#   53 ef
```

So the filesystems are at 8 MiB and 244 MiB, which is exactly the layout the
first working install used. The table values are off by a factor of 8.

## Cause

`partition_install_device()` in `chroot/bin/pmos_install_functions`:

```sh
for command in "mktable msdos" \
    "mkpart primary ext2 2048s 256M" \
    "mkpart primary 256M 100%" \
    "set 1 boot on"
do
    parted -s "$INSTALL_DEVICE" "$command"
done
```

`2048s` means 2048 sectors, and parted takes the sector size from the
device. This phone's `userdata` is a 4Kn device: 4096-byte logical sectors.
So parted resolves `2048s` to 8 MiB, which is what it should mean, and then
writes `2048` into the table, which a 512-byte reader reads as 1 MiB. The
value in the table and the value parted meant are not the same number, and
nothing catches the discrepancy.

The filesystems get created at the intended byte offsets by mkfs, so they and
the table disagree by exactly 8x.

## Fix

`tools/pmos-patch-ab.sh` now injects a third patch into
`pmos_install_functions`:

```sh
lind_fix_mbr_4kn() {
    _d="$1"
    _lb=$(cat "/sys/block/$(basename "$_d")/queue/logical_block_size" 2>/dev/null || echo 512)
    [ "$_lb" = "4096" ] || return 0
    dd if="$_d" bs=1 skip=446 count=64 2>/dev/null | od -An -v -tu1 | awk '
        function le32(o) { return b[o] + b[o+1]*256 + b[o+2]*65536 + b[o+3]*16777216 }
        function put32(o, v) { b[o]=v%256; b[o+1]=int(v/256)%256; b[o+2]=int(v/65536)%256; b[o+3]=int(v/16777216)%256 }
        { for (i = 1; i <= NF; i++) b[n++] = $i }
        END {
            for (e = 0; e < 64; e += 16) {
                s = le32(e + 8); c = le32(e + 12)
                if (s > 0) put32(e + 8, s * 8)
                if (c > 0) put32(e + 12, c * 8)
            }
            for (i = 0; i < 64; i++) printf "%c", b[i]
        }' > /tmp/lind_mbr.bin
    ...
    dd if=/tmp/lind_mbr.bin of="$_d" bs=1 seek=446 count=64 conv=fsync
}
```

It runs between parted and `partprobe`, so the kernel only ever sees the
corrected table. It is a no-op on any device whose logical sector is 512.

Verified against the bytes actually read off the phone:

| | before | after |
|---|---|---|
| p1 | LBA 2048, 60416 sectors | LBA 16384, 483328 sectors |
| p2 | LBA 62464, 58313472 sectors | LBA 499712, 466507776 sectors |
| p1 at | 1 MiB | 8 MiB |
| p2 at | 30.5 MiB | 244 MiB |

## Confirming a fix

```sh
losetup -d /dev/loop1
losetup --show -Pf /dev/sda19
mdev -s
blkid /dev/loop1p1 /dev/loop1p2
```

```
/dev/loop1p1: LABEL="pmOS_boot" ... TYPE="ext2"
/dev/loop1p2: LABEL="pmOS_root" ... TYPE="ext4"
```

Labels present is the signal. `find_root_partition` looks the root up by
label:

```sh
partition="$(blkid --label "pmOS_root")"
```

so without it `PMOS_ROOT` stays empty and the loop is detached and retried
until the ten second limit expires.

## The manual fix, if the table is already wrong

From the initramfs debug shell, rescale it by hand:

```sh
echo gOMGAIM3BBsAQAAAAGAHAAAAAACD/sn/AKAHAABYzhsAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA== \
  | base64 -d | dd of=/dev/sda19 bs=1 seek=446 count=64 conv=fsync
sync
```

The base64 is the corrected 64-byte table. This is what was done to recover
the phone; the installer patch is what prevents it from happening again.
