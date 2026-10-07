#!/usr/bin/env bash
#
# pmos-initramfs.sh - build a bootable pmOS boot image for this phone.
#
# What this produces, and why each piece is the way it is. Everything here was
# measured on the device; see docs/pmos-boot-failure.md and docs/pmos-dtb.md.
#
# 1. The boot image is built from the STOCK boot.img, never from the pmOS one.
#    Both images that work on this phone - stock boot_a and the working boot_b -
#    are `magiskboot repack` of stock, so their header is the stock header
#    copied verbatim, header_version 3 and page_size 0 and an empty cmdline. A
#    boot image built from scratch by mkbootimg gets header_version 0 and
#    page_size 4096, and this bootloader silently refuses to boot it: no output,
#    no fastboot, straight back to the bootloader. pmOS documents boot.img as
#    "header <=2" precisely because of this split.
#
# 2. The kernel is ours. Verified by md5 after every build.
#
# 3. The initramfs is the pmOS one with /init replaced, because the pmOS init
#    cannot finish on this device. Two reasons, both from the running system:
#
#    a) No udev. `# CONFIG_DEVTMPFS is not set` and CONFIG_UEVENT_FS unset, so
#       nothing publishes device events and nothing creates /dev/block/by-name or
#       /dev/disk/by-partlabel. pmOS's mount_subpartitions() looks only at
#       /dev/disk/by-partlabel/userdata, /dev/block/sde19 and /dev/mapper/system*,
#       all of which are empty, so the partitions inside userdata are never
#       created and pmOS_root is never found. The installer's `findfs
#       PARTLABEL=boot_a` fails for the same reason, which is why it wrote
#       boot.img into an empty variable.
#
#    b) `losetup -P` alone is not enough. It does make the kernel parse the
#       table - loop0p1 and loop0p2 show up in /proc/partitions - but the device
#       nodes are never created, because the mdev run happened earlier. blkid
#       can only probe nodes, so it finds no pmOS_root. Hence: run mdev again
#       after losetup, and mknod anything still missing, taking major and minor
#       straight out of /proc/partitions.
#
# 4. There is no console. CONFIG_FRAMEBUFFER_CONSOLE, CONFIG_VT and
#    CONFIG_DRM_FBDEV_EMULATION are all unset, so there is no tty0, and the
#    stock cmdline is empty so /dev/console goes nowhere. The only way to see
#    anything is to write into /dev/ttyGS0 directly, after userspace creates the
#    ACM gadget function.
#
# 5. Output is emitted one line at a time with a pause. A single `exec >/dev/
#    ttyGS0` followed by ordinary commands kills the phone - it reboots about
#    three seconds after the gadget comes up. Two one-byte writes are fine, and
#    twelve kilobytes line by line is fine. So: no redirection, no command
#    substitution into the port, a sleep between lines.
#
# Usage:
#   pmos-initramfs.sh [out.img]
#
# SPDX-License-Identifier: GPL-2.0-only

set -e

OUTIMG="${1:-$HOME/lindroid-kernel/out/boot-pmos.img}"
STOCK="$HOME/kernel_xiaomi_stone/stock/boot.img"
OUR_IMAGE="$HOME/ak3-darkmoon/Image"
PMOS_INITRAMFS="$HOME/pmos_kernel/chroot_rootfs_xiaomi-moonstone/boot/initramfs"
MAGISKBOOT="${MAGISKBOOT:-$HOME/ak3-darkmoon/tools/magiskboot}"

say()  { printf '%s\n' "$*"; }
die()  { printf 'ошибка: %s\n' "$*" >&2; exit 1; }

for f in "$STOCK" "$OUR_IMAGE" "$PMOS_INITRAMFS" "$MAGISKBOOT"; do
	[ -f "$f" ] || die "нет $f"
done
mkdir -p "$(dirname "$OUTIMG")"

mb() {
	case "$(file -b "$MAGISKBOOT")" in
	*ARM*) qemu-arm "$MAGISKBOOT" "$@" ;;
	*)     "$MAGISKBOOT" "$@" ;;
	esac
}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

say "=== распаковываю pmOS initramfs"
RD="$WORK/rd"
mkdir -p "$RD"
( cd "$RD" && gzip -dc "$PMOS_INITRAMFS" | cpio -idm --quiet ) 2>/dev/null ||
	die "не распаковался initramfs"
[ -x "$RD/usr/bin/busybox" ] || die "в initramfs нет busybox"

say "=== заменяю /init"
cat > "$RD/init" <<'INIT'
#!/bin/busybox ash
# Отладочный init для pmOS на этом телефоне. Полностью заменяет init pmOS -
# см. tools/pmos-initramfs.sh, там же почему.

export PATH=/usr/sbin:/usr/bin:/sbin:/bin
/bin/busybox --install -s

mount -t proc     -o nodev,noexec,nosuid proc     /proc 2>/dev/null
mount -t sysfs    -o nodev,noexec,nosuid sysfs    /sys  2>/dev/null
mount -t tmpfs    -o mode=0755,nosuid tmpfs      /dev  2>/dev/null
mkdir -p /dev/pts
mount -t devpts   -o mode=0620,gid=5 devpts /dev/pts 2>/dev/null
mount -t configfs -o nodev,noexec,nosuid configfs /sys/kernel/config 2>/dev/null

# Без CONFIG_UEVENT_FS ядро сообщает о новых устройствах только через эту
# переменную, а не через netlink. Пока её не задали, узлы не появляются ни для
# чего - включая /dev/ttyGS0, который создаётся в самом конце этого скрипта.
echo /bin/mdev > /proc/sys/kernel/hotplug 2>/dev/null
# mdev вызывает /lib/mdev/persistent-storage на каждый узел, а его в initramfs
# нет - и на каждый вызов сыплется ошибка, забивая вывод. Пустой файл вместо
# него снимает весь шум.
mkdir -p /lib/mdev && : > /lib/mdev/persistent-storage
mdev -s 2>/dev/null

# Симлинки, которые обычно делает udev. Без них ни установщик, ни
# mount_subpartitions() ничего не находят.
mkdir -p /dev/block/by-name /dev/disk/by-partlabel /dev/disk/by-partuuid
blkid 2>/dev/null | while read -r dev rest; do
	[ -b "$dev" ] || continue
	pl=$(echo "$rest" | sed -n 's/.*PARTLABEL="\([^"]*\)".*/\1/p')
	pu=$(echo "$rest" | sed -n 's/.*PARTUUID="\([^"]*\)".*/\1/p')
	[ -n "$pl" ] && ln -sf "$dev" "/dev/block/by-name/$pl" 2>/dev/null
	[ -n "$pl" ] && ln -sf "$dev" "/dev/disk/by-partlabel/$pl" 2>/dev/null
	[ -n "$pu" ] && ln -sf "$dev" "/dev/disk/by-partuuid/$pu" 2>/dev/null
done

# Gadget с ACM - единственный способ получить консоль.
G=/sys/kernel/config/usb_gadget/lind
UDC=$(ls /sys/class/udc 2>/dev/null | head -1)
mkdir -p "$G/strings/0x409" "$G/functions/acm.GS0" "$G/configs/c.1/strings/0x409" 2>/dev/null
echo 0x18d1 > "$G/idVendor" 2>/dev/null
echo 0xd001 > "$G/idProduct" 2>/dev/null
echo "lindroid" > "$G/strings/0x409/serialnumber" 2>/dev/null
echo "debug"   > "$G/configs/c.1/strings/0x409/configuration" 2>/dev/null
ln -s "$G/functions/acm.GS0" "$G/configs/c.1/acm.usb" 2>/dev/null
echo "$UDC" > "$G/UDC" 2>/dev/null
mdev -s
sleep 4

# Вывод строка за строкой с паузой. Никакого exec >/dev/ttyGS0: перенаправление
# убивает телефон. И никаких подстановок команд прямо в порт.
emit() {
	while IFS= read -r l; do
		printf '%s\n' "$l" > /dev/ttyGS0 2>/dev/null
		sleep 0.12
	done < "$1"
}

report() {
	{
		echo "=== $(uname -r) ==="
		echo "--- ttyGS0 ---";  ls -l /dev/ttyGS0 2>&1
		echo "--- by-name ---";  ls /dev/block/by-name/ 2>/dev/null | head -12
		echo "--- partlabel ---"; ls /dev/disk/by-partlabel/ 2>/dev/null | head -12
		echo "--- sda19 ---";    blkid /dev/sda19 2>&1
		echo "--- dri ---";      ls /dev/dri/ 2>&1
		echo "--- modules ---";  ls /lib/modules/ 2>&1
		echo "--- losetup -P ---"
		LOOP=$(losetup -f 2>/dev/null)
		echo "free loop: [$LOOP]"
		losetup -P "$LOOP" /dev/sda19 2>&1
		sleep 3
		# Вот это и было главным: разделы ядро создало, а узлы - нет.
		mdev -s 2>/dev/null
		for p in $(grep -E "loop[0-9]+p[0-9]+" /proc/partitions | awk '{print $4}'); do
			maj=$(awk -v p="$p" '$4==p {print $1}' /proc/partitions)
			min=$(awk -v p="$p" '$4==p {print $2}' /proc/partitions)
			if [ -n "$maj" ] && [ ! -b "/dev/$p" ]; then
				mknod "/dev/$p" b "$maj" "$min" 2>/dev/null &&
					echo "создан /dev/$p b $maj $min"
			fi
		done
		mdev -s 2>/dev/null
		sleep 2
		echo "--- losetup -a ---"; losetup -a 2>&1
		echo "--- loop parts ---"; ls /dev/loop* 2>&1
		echo "--- pmOS в blkid ---"; blkid 2>/dev/null | grep -i pmOS
		echo "--- blkid по узлам ---"
		for p in $(grep -E "loop[0-9]+p[0-9]+" /proc/partitions | awk '{print $4}'); do
			echo "[$p] $(blkid /dev/$p 2>&1)"
		done
		echo "--- суперблок ext по 1080 от начала раздела ---"
		for p in $(grep -E "loop[0-9]+p[0-9]+" /proc/partitions | awk '{print $4}'); do
			echo "[$p] $(dd if=/dev/$p bs=1 skip=1080 count=2 2>/dev/null | od -An -tx1 | tr -d ' \n')"
		done
		echo "--- partitions ---"; cat /proc/partitions 2>/dev/null | grep -E "loop|sda19"
	} > /tmp/r1.txt 2>&1
	emit /tmp/r1.txt
}

# Монтирование найденного корня.
mountroot() {
	{
		ROOT=$(blkid 2>/dev/null | grep pmOS_root | cut -d: -f1)
		echo "ROOT=[$ROOT]"
		BOOTP=$(blkid 2>/dev/null | grep pmOS_boot | cut -d: -f1)
		echo "BOOT=[$BOOTP]"
		if [ -n "$ROOT" ]; then
			mkdir -p /mnt/root
			if mount -t ext4 -o ro "$ROOT" /mnt/root 2>&1; then
				echo "--- /mnt/root ---"; ls /mnt/root 2>&1 | head -20
				echo "--- os-release ---"; cat /mnt/root/etc/os-release 2>&1 | head -4
				echo "--- init ---"; ls -l /mnt/root/sbin/init 2>&1
				echo "--- lib/modules ---"; ls /mnt/root/lib/modules/ 2>&1 | head -4
			else
				echo "mount не удался"
			fi
		else
			echo "pmOS_root не найден"
		fi
	} > /tmp/r2.txt 2>&1
	emit /tmp/r2.txt
}

report
mountroot

# Дальше - команды с порта. Заодно это способ выключить телефон, потому что
# в fastboot команды выключения нет, а long-press Power не всегда удобен.
# Без этого read выдаёт по 15 символов вместо строки: tty не в каноническом
# режиме, и команда "report" приходит кусками.
stty -F /dev/ttyGS0 sane 2>/dev/null || stty -F /dev/ttyGS0 -icanon 2>/dev/null
echo "=== жду команду (off/reboot/ls/mount) ===" > /tmp/r3.txt
emit /tmp/r3.txt
while read -r line; do
	case "$line" in
		off)    emit /tmp/r3.txt; poweroff -f ;;
		reboot) poweroff -f ;;
		mount)  mountroot ;;
		report) report ;;
		*)      printf 'не знаю: %s\n' "$line" > /dev/ttyGS0 2>/dev/null ;;
	esac
done < /dev/ttyGS0
INIT
chmod 755 "$RD/init"

say "=== собираю initramfs"
( cd "$RD" && find . | cpio -o -H newc --owner=0:0 --quiet ) < /dev/null > "$WORK/initramfs.cpio"
gzip -1 -c "$WORK/initramfs.cpio" > "$WORK/initramfs"
say "  initramfs: $(stat -c%s "$WORK/initramfs") байт"

say "=== собираю boot.img от стокового заголовка"
( cd "$WORK" && mb unpack -h "$STOCK" ) >/dev/null 2>&1 || die "magiskboot не распаковал стоковый образ"
cp "$OUR_IMAGE" "$WORK/kernel"
cp "$WORK/initramfs" "$WORK/ramdisk.cpio"
printf 'cmdline=\nos_version=16.0.0\nos_patch_level=2026-04\n' > "$WORK/header"
( cd "$WORK" && mb repack "$STOCK" "$OUTIMG" ) >/dev/null 2>&1 || die "magiskboot не пересобрал"

say ""
say "=== проверка"
V="$WORK/verify"; mkdir -p "$V"
( cd "$V" && mb unpack -h "$OUTIMG" ) >/dev/null 2>&1
python3 - "$V" "$OUR_IMAGE" <<'PY'
import hashlib, sys
v, ours = sys.argv[1], sys.argv[2]
k = open(f'{v}/kernel', 'rb').read()
o = open(ours, 'rb').read()
print(f'  ядро == наш Image: {"ДА" if k == o else "НЕТ"} ({len(k)} байт)')
if k != o:
    sys.exit('ОШИБКА: ядро не то, не прошивать')
PY
python3 - "$OUTIMG" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read(48)
f = lambda o: int.from_bytes(d[o:o+4], 'little')
print(f'  header_version={f(40)} page_size={f(36)} kernel_size={f(8)}')
assert f(40) == 3, 'заголовок не v3 - bootloader такое не грузит'
print('  заголовок v3 как у рабочих образов: ДА')
PY

say ""
say "готово: $OUTIMG ($(stat -c%s "$OUTIMG") байт)"
say ""
say "прошить:"
say "  fastboot flash boot_a $OUTIMG"
say "  fastboot reboot"
say "читать вывод:"
say "  python3 $(dirname "$0")/lind-console.py 120"