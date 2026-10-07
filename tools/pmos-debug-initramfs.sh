#!/usr/bin/env bash
#
# pmos-debug-initramfs.sh - replace the pmOS initramfs with a minimal debug one.
#
# Why
# ---
# The kernel boots and something fails, but there is no way to see it:
#
#   - CONFIG_FRAMEBUFFER_CONSOLE, CONFIG_VT and CONFIG_DRM_FBDEV_EMULATION are
#     not set in our .config, so there is no tty0 at all. `console=tty0` cannot
#     work; that is not a choice, the driver does not exist.
#   - console=ttyGS0 only becomes a real port once userspace creates the ACM
#     gadget function. The pmOS initramfs does that in
#     setup_usb_network_configfs(), which is called from setup_usb_network() at
#     init line 53 - after mount_proc_sys_dev, the 2nd stage init jump, mdev,
#     module loading, USB networking and unudhcpd. Any of those can fail, and
#     everything before the gadget exists is unobservable.
#
# So replace /init with something that does the minimum: mount the pseudo
# filesystems, build the gadget, print the kernel command line, and hand the
# console to a shell. Everything pmOS does is dropped, so what is left failing
# is the kernel or the device tree, not the initramfs.
#
# The busybox and its musl libraries are not touched - only /init is replaced -
# so this stays a musl userland and keeps working.
#
# Note there is no devtmpfs in this kernel either (CONFIG_DEVTMPFS unset), so
# /dev is a tmpfs populated by mdev, the same way pmOS does it.
#
# Usage:
#   pmos-debug-initramfs.sh <boot.img> [out.img]
#
# SPDX-License-Identifier: GPL-2.0-only

set -e

BOOTIMG="${1:?нужен путь к boot.img}"
OUTIMG="${2:-${BOOTIMG%.img}-dbg.img}"
MAGISKBOOT="${MAGISKBOOT:-$HOME/ak3-darkmoon/tools/magiskboot}"
CONSOLE="ttyGS0,115200n8"

say() { printf '%s\n' "$*"; }
die() { printf 'ошибка: %s\n' "$*" >&2; exit 1; }

[ -f "$BOOTIMG" ] || die "нет $BOOTIMG"
[ -f "$MAGISKBOOT" ] || die "нет magiskboot"

mb() {
	case "$(file -b "$MAGISKBOOT")" in
	*ARM*) qemu-arm "$MAGISKBOOT" "$@" ;;
	*)     "$MAGISKBOOT" "$@" ;;
	esac
}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

say "=== распаковываю $BOOTIMG"
( cd "$WORK" && mb unpack -h "$BOOTIMG" ) >/dev/null 2>&1 ||
	die "magiskboot не распаковал"
[ -f "$WORK/ramdisk.cpio" ] || die "в образе нет ramdisk.cpio"

RD="$WORK/rd"
mkdir -p "$RD"
( cd "$RD" && cpio -idm --quiet < "$WORK/ramdisk.cpio" ) 2>/dev/null ||
	die "cpio не распаковал ramdisk"

say "=== проверяю, что userland на месте"
for f in usr/bin/busybox lib/ld-musl-aarch64.so.1; do
	[ -e "$RD/$f" ] || die "в initramfs нет $f"
done
[ -x "$RD/usr/bin/busybox" ] || die "busybox не исполняемый"

say "=== заменяю /init на отладочный"
cp -p "$RD/init" "$WORK/init.pmOS.bak"

cat > "$RD/init" <<'INIT'
#!/bin/busybox ash
# Отладочный init: только минимум, чтобы получить консоль на USB.
# Заменяет pmOS init целиком - см. tools/pmos-debug-initramfs.sh

export PATH=/usr/sbin:/usr/bin:/sbin:/bin
/bin/busybox --install -s

say() { echo "[dbg] $*"; }

say "=== отладочный init, ядро запустилось ==="
uname -a
say "cmdline: $(cat /proc/cmdline)"
say "жёсткая память: $(grep MemTotal /proc/meminfo)"

# Всё, что напечатано выше, ушло в /dev/console, а он никуда не ведёт: у этого
# образа cmdline пустой, как у стокового, и console= в нём нет. Поэтому после
# создания gadget перенаправляем вывод прямо в /dev/ttyGS0 - ядро для этого не
# нужно, достаточно того, что порт существует.

mount -t proc     -o nodev,noexec,nosuid proc     /proc 2>/dev/null
mount -t sysfs    -o nodev,noexec,nosuid sysfs    /sys  2>/dev/null
mount -t tmpfs    -o mode=0755,nosuid,nodev tmpfs /dev  2>/dev/null
mkdir -p /dev/pts /dev/shm
mount -t devpts   -o mode=0620,gid=5 devpts /dev/pts 2>/dev/null
mount -t configfs -o nodev,noexec,nosuid configfs /sys/kernel/config 2>/dev/null
echo /dev/mdev > /proc/sys/kernel/hotplug 2>/dev/null
mdev -s

say "модулей ядра нет: $(ls /lib/modules 2>/dev/null | tr '\n' ' ')"
say "разделы: $(ls /dev/block/ 2>/dev/null | grep -cE '^(mmc|sd|dm)' ) штук"

# Gadget только с ACM: этого достаточно, чтобы /dev/ttyGS0 появился.
G=/sys/kernel/config/usb_gadget/lind
if [ -d /sys/kernel/config/usb_gadget ]; then
	UDC=$(ls /sys/class/udc 2>/dev/null | head -1)
	say "UDC: [$UDC]"
	mkdir -p "$G" 2>/dev/null
	echo 0x18d1 > "$G/idVendor" 2>/dev/null
	echo 0xd001 > "$G/idProduct" 2>/dev/null
	mkdir -p "$G/strings/0x409" "$G/functions/acm.GS0" \
		"$G/configs/c.1/strings/0x409" 2>/dev/null
	echo "lind-debug" > "$G/strings/0x409/serialnumber" 2>/dev/null
	echo "debug"     > "$G/configs/c.1/strings/0x409/configuration" 2>/dev/null
	ln -s "$G/functions/acm.GS0" "$G/configs/c.1/acm.usb" 2>/dev/null
	echo "$UDC" > "$G/UDC" 2>/dev/null && say "UDC подключён" || say "UDC НЕ подключился"
	sleep 2
	# Узла /dev/ttyGS0 не будет само: CONFIG_DEVTMPFS не задан, в этом ядре
	# узлы создаёт только mdev, а он отработал выше - до создания gadget.
	# Из-за этого exec >/dev/ttyGS0 падал, init доходил до конца, умирал, и
	# телефон уходил в перезагрузку: логотип, чёрный экран, ребут.
	echo /bin/mdev > /proc/sys/kernel/hotplug 2>/dev/null
	mdev -s
	sleep 1
	# Major у ttyGS динамический: 166 - это ttyUSB, жёстко писать его
	# бесполезно. Настоящий major лежит в /proc/devices.
	if [ ! -e /dev/ttyGS0 ]; then
		MAJ=$(awk '/ttyGS/{print $1}' /proc/devices 2>/dev/null | head -1)
		if [ -n "$MAJ" ]; then
			mknod /dev/ttyGS0 c "$MAJ" 0 2>/dev/null &&
				say "узел создан: mknod /dev/ttyGS0 c $MAJ 0"
		else
			say "ttyGS в /proc/devices нет"
		fi
	fi
	[ -e /dev/ttyGS0 ] && say "/dev/ttyGS0 есть" || say "/dev/ttyGS0 НЕТ"
else
	say "configfs не смонтирован - gadget невозможен"
fi

# Теперь порт есть - дальше весь вывод идёт в него напрямую.
if [ -e /dev/ttyGS0 ]; then
	exec >/dev/ttyGS0 2>&1
	echo ""
	echo "=== вывод перенаправлен в /dev/ttyGS0 ==="
	echo "=== состояние ядра ==="
	uname -a
	echo "cmdline: $(cat /proc/cmdline)"
	echo ""
	echo "=== разделы, которые видит ядро ==="
	ls /dev/block/ 2>/dev/null | grep -E '^(mmc|sd|dm)' | head -20
	echo ""
	echo "=== то, что нужно для отладки ==="
	echo "модулей в rootfs нет: $(ls /lib/modules 2>/dev/null | tr '\n' ' ')"
	echo "drm: $(ls /dev/dri/ 2>/dev/null | tr '\n' ' ')"
	echo "файловых систем: $(grep -c . /proc/filesystems)"
	echo ""
	echo "=== pmOS_root и pmOS_boot найдутся? ==="
	blkid 2>/dev/null | grep -i pmOS || echo "  blkid ничего не нашёл"
	echo ""
	echo "=== отдал консоль, жду команды ==="
	exec setsid sh -c 'exec sh -i 0</dev/ttyGS0 1>/dev/ttyGS0 2>&1'
else
	# Порта нет. Умирать нельзя: init, который завершился, вызывает
	# "Attempted to kill init" и телефон уходит в перезагрузку - так теряется
	# всё, что мы уже выяснили. Лучше висеть с поднятым gadget: телефон грузится,
	# порт на хосте виден как ttyACM, и можно хотя бы посмотреть состояние.
	echo "порта нет - висну, чтобы не перезагружаться"
	while true; do sleep 30; done
fi
INIT
chmod 755 "$RD/init"

say "=== собираю initramfs"
( cd "$RD" && find . | cpio -o -H newc --owner=0:0 --quiet ) < /dev/null > "$WORK/ramdisk.cpio"
say "  ramdisk.cpio: $(stat -c%s "$WORK/ramdisk.cpio") байт"

say "=== cmdline"
CMD=$(grep '^cmdline=' "$WORK/header" | cut -d= -f2-)
case "$CMD" in
*console=*) CMD=$(printf '%s' "$CMD" | sed -E 's/(^| )console=[^ ]*/\1/g; s/  */ /g; s/^ //; s/ $//') ;;
esac
printf 'cmdline=%s console=%s\n' "$CMD" "$CONSOLE" > "$WORK/header"
grep '^cmdline=' "$WORK/header" | sed 's/^/  /'

( cd "$WORK" && mb repack "$BOOTIMG" "$OUTIMG" ) >/dev/null 2>&1 ||
	die "magiskboot не пересобрал"

say ""
say "=== готово: $OUTIMG ($(stat -c%s "$OUTIMG") байт)"
say "запись на телефон:"
say "  adb push $OUTIMG /tmp/dbg.img"
say "  adb shell 'dd if=/tmp/dbg.img of=/dev/block/by-name/boot_a bs=4M conv=fsync'"
say "  adb reboot   # затем на PC: screen /dev/ttyACM0 115200"