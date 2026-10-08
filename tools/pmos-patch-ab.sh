#!/usr/bin/env bash
#
# pmos-patch-ab.sh - make a pmOS recovery zip work on an A/B Android phone.
#
# Why
# ---
# The pmOS recovery installer has no notion of A/B slots. Not one mention of
# "slot" in chroot/bin/pmos_install, pmos_install_functions or install_options.
# It resolves its target like this:
#
#     dev=$(findfs PARTLABEL="$INSTALL_PARTITION") || \
#             dev=$(readlink -fn "$(get_fstab_device "$fstab_recovery" "$INSTALL_PARTITION")")
#
# PARTLABEL here is the GPT partition name. On this phone the partitions are
# named boot_a, boot_b, userdata, vendor_boot_a, ... and there is no partition
# simply named "boot":
#
#     /dev/block/by-name/boot_a   -> /dev/block/sde9
#     /dev/block/by-name/userdata -> /dev/block/sda19
#     /dev/block/by-name/boot     -> No such file or directory
#
# So findfs fails, the fallback reads /recovery.fstab, which AOSP recovery does
# not have at the root (it keeps its fstab at /system/etc/recovery.fstab and has
# no /boot entry anyway), dev ends up empty, and the installer dies at its
# first step having written nothing. That is the
#
#     findfs: unable to resolve 'PARTLABEL=boot'
#
# in /tmp/postmarketos/pmos.log.
#
# What the wiki says, and what it does not cover
# ----------------------------------------------
# https://wiki.postmarketos.org/wiki/Ports_and_PostmarketOS#Modifying_for_A/B_devices
# says to replace "boot" with "boot_a" and "system" with "system_a". For this
# phone the install target must not be boot_a. partition_install_device()
# repartitions whatever it is given:
#
#     mktable msdos
#     mkpart primary ext2 2048s 256M     -> p1, pmOS_boot
#     mkpart primary 256M 100%           -> p2, pmOS_root
#
# boot_a is 128 MiB, so a pmOS root inside it would be about 128 MiB for a
# 1.5 GB rootfs. userdata is 239 GB and is not slotted on this device, so it is
# the right target - and it is what the initramfs already expects, since
# mount_subpartitions() in init_functions.sh tries
# /dev/disk/by-partlabel/userdata first.
#
# Two edits therefore:
#
#   chroot/install_options           INSTALL_PARTITION  boot -> userdata
#   chroot/bin/pmos_install_functions the one hardcoded
#                                    findfs PARTLABEL="boot" -> "boot_a"
#
# The second one must not be changed to "$INSTALL_PARTITION": that variable is
# the install target, and BOOT_PARTITION is a different device - it is where
# boot.img gets dd'd, and that is the phone's real boot partition.
#
# Only these two text files are touched. rootfs.tar.gz holds our boot.img and is
# left byte for byte alone, so this can be re-run on an already-patched zip.
#
# Usage:
#   pmos-patch-ab.sh <pmos.zip> [slot]     # slot defaults to _a

set -e

ZIP="${1:?нужен путь к pmos zip}"
SLOT="${2:-_a}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

say() { printf '%s\n' "$*"; }
die() { printf 'ошибка: %s\n' "$*" >&2; exit 1; }

[ -f "$ZIP" ] || die "нет $ZIP"
case "$SLOT" in
_a | _b) ;;
*) die "слот должен быть _a или _b, не '$SLOT'" ;;
esac

BOOTPART="boot$SLOT"

say "=== zip: $ZIP"
say "=== слот: $SLOT, раздел загрузки: $BOOTPART"

unzip -q -o "$ZIP" chroot/install_options chroot/bin/pmos_install_functions -d "$WORK"
F_OPT="$WORK/chroot/install_options"
F_FUN="$WORK/chroot/bin/pmos_install_functions"
[ -f "$F_OPT" ] || die "в zip нет chroot/install_options"
[ -f "$F_FUN" ] || die "в zip нет chroot/bin/pmos_install_functions"

say "=== было"
grep -n "INSTALL_PARTITION" "$F_OPT" | sed 's/^/  /'
grep -n 'PARTLABEL="boot"' "$F_FUN" | sed 's/^/  /'

# 1. install target: userdata, not boot_a
python3 - "$F_OPT" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
new, n = re.subn(r"INSTALL_PARTITION='[^']*'", "INSTALL_PARTITION='userdata'", s, count=1)
if n != 1:
    sys.exit('ОШИБКА: не нашёл INSTALL_PARTITION')
open(p, 'w', encoding='utf-8').write(new)
PY

# 2. BOOT_PARTITION: the phone's real boot partition for this slot.
#    Only the hardcoded "boot" is replaced; the one that already uses
#    $INSTALL_PARTITION must stay as it is.
python3 - "$F_FUN" "$BOOTPART" <<'PY'
import re, sys
p, part = sys.argv[1], sys.argv[2]
s = open(p, encoding='utf-8').read()
hard = s.count('findfs PARTLABEL="boot"')
if hard != 1:
    sys.exit(f'ОШИБКА: жёсткий findfs PARTLABEL="boot" встречается {hard} раз, ожидался 1')
s = s.replace('findfs PARTLABEL="boot"', f'findfs PARTLABEL="{part}"')
s = s.replace('get_fstab_device "$fstab_recovery" boot)',
              f'get_fstab_device "$fstab_recovery" {part})')
open(p, 'w', encoding='utf-8').write(s)
print(f'  BOOT_PARTITION -> {part}')
PY

# 3. msdos table on a 4Kn device.
#    parted writes the partition table in units of the device's logical sector,
#    which on this phone is 4096 bytes, but the msdos format stores LBAs in
#    512-byte units and nothing rescales them. The result is a table that is
#    off by a factor of 8: parted meant p1 to start at 2048s = 8 MiB and it
#    wrote 2048, which a 512-byte reader takes as 1 MiB.
#
#    The filesystems are created at the right byte offsets anyway, so the
#    kernel finds the table, mounts partitions at the wrong places, and
#    pmOS's mount_subpartitions() never sees pmOS_root. Measured on the phone:
#
#        fdisk -l /dev/sda19      p1 2048    60416   p2 62464  58313472
#        magic at 8 MiB + 1080    53 ef     <- the real ext2 superblock
#        magic at 30.5 MiB +1080  1a f8     <- what the table points at
#
#    Rescaling by 8 puts p1 at 8 MiB and p2 at 244 MiB, which is where the
#    filesystems really are, and is the same layout the working install used.
python3 - "$F_FUN" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()

FUNC = r'''
# --- lind: parted on a 4Kn device writes 4096-byte LBAs into the msdos table
# The msdos format stores LBAs in 512-byte units, and nothing rescales them.
# On a device whose logical sector is 4096 bytes the table therefore lands
# 8x too low and the kernel mounts the partitions at the wrong offsets.
lind_fix_mbr_4kn() {
	_d="$1"
	_lb=$(cat "/sys/block/$(basename "$_d")/queue/logical_block_size" 2>/dev/null || echo 512)
	[ "$_lb" = "4096" ] || return 0
	echo "lind: logical sector is 4096, rescaling the msdos table by 8"
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
	}' > /tmp/lind_mbr.bin 2>/dev/null
	if [ ! -s /tmp/lind_mbr.bin ]; then
		echo "lind: rescale produced nothing, leaving the table untouched"
		return 0
	fi
	dd if=/tmp/lind_mbr.bin of="$_d" bs=1 seek=446 count=64 conv=fsync 2>/dev/null
	rm -f /tmp/lind_mbr.bin
	echo "lind: table rescaled"
}
'''

if 'lind_fix_mbr_4kn' not in s:
    s = s.replace('\npartition_install_device() {', FUNC + '\npartition_install_device() {', 1)
    old = '\tdone\n\tpartprobe\n'
    new = '\tdone\n\tlind_fix_mbr_4kn "$INSTALL_DEVICE"\n\tpartprobe\n'
    if old not in s:
        sys.exit('ОШИБКА: не нашёл "done\\n\\tpartprobe" в partition_install_device')
    s = s.replace(old, new, 1)
    print('  lind_fix_mbr_4kn добавлена в partition_install_device')
else:
    print('  lind_fix_mbr_4kn уже на месте')

open(p, 'w', encoding='utf-8').write(s)
PY

say "=== стало"
grep -n "INSTALL_PARTITION" "$F_OPT" | sed 's/^/  /'
grep -n "PARTLABEL=" "$F_FUN" | sed 's/^/  /'

say "=== обновляю zip"
( cd "$WORK" && zip -q "$ZIP" chroot/install_options chroot/bin/pmos_install_functions )

say ""
say "=== проверка"
unzip -p "$ZIP" chroot/install_options | grep -E "INSTALL_PARTITION|FLASH_KERNEL|ISOREC" | sed 's/^/  /'
unzip -p "$ZIP" chroot/bin/pmos_install_functions |
	grep -n 'findfs PARTLABEL' | sed 's/^/  /'
unzip -t "$ZIP" >/dev/null 2>&1 && say "  целостность zip: OK" || die "zip повреждён"

# The boot image we injected must still be ours.
say "=== наше ядро на месте?"
unzip -p "$ZIP" rootfs.tar.gz 2>/dev/null |
	tar -xzO ./boot/boot.img 2>/dev/null > "$WORK/b.img" || true
if [ -s "$WORK/b.img" ]; then
	rm -rf "$WORK/u"; mkdir -p "$WORK/u"
	( cd "$WORK/u" && qemu-arm "$HOME/ak3-darkmoon/tools/magiskboot" unpack "$WORK/b.img" ) >/dev/null 2>&1
	grep -ao "5\.4\.302-Darkmoon-Reborn" "$WORK/u/kernel" 2>/dev/null | head -1 |
		sed 's/^/  /' || say "  ВНИМАНИЕ: не нашла строку версии в ядре"
else
	say "  ВНИМАНИЕ: не удалось достать boot.img из архива"
fi

say ""
say "готово: $ZIP"
say "sideload: adb sideload $ZIP"
say "в recovery: Apply update / Install zip, либо adb sideload с ПК"