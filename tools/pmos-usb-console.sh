#!/usr/bin/env bash
#
# pmos-usb-console.sh - give a pmOS initramfs a console over the USB port.
#
# The problem
# -----------
# pmOS cannot be reached after a flash. There is no adb, no network driver
# (the rootfs carries modules for mainline 7.2.0 while the kernel is our
# 5.4.302), and the panel needs SDE, so there is no framebuffer console either.
# With no console= on the command line the kernel boots, the initramfs runs,
# and nobody can see any of it.
#
# Why no hook
# -----------
# init_functions.sh defines run_hooks() and nothing calls it, and there is no
# /hooks directory in this initramfs. A hook dropped in there would sit there
# forever. So the change is made to the function that already builds the gadget.
#
# What is already there
# ---------------------
# pmOS mounts configfs and builds a USB gadget by itself, in
# setup_usb_network_configfs() called from setup_usb_network() in init:
#
#   - gadget g1, idVendor 0x18D1, idProduct 0xD001
#   - one network function: ncm.usb0, falling back to rndis.usb0
#   - UDC bind, then unudhcpd hands out 172.16.42.2
#
# What is missing is a serial function. With CONFIG_USB_CONFIGFS_ACM=y in our
# kernel (verified in out/.config) adding one is enough to get /dev/ttyGS0, and
# the phone then shows up on the PC as /dev/ttyACM0 with no hardware at all.
#
# The kernel is told about it on the command line, console=ttyGS0. That is fine
# even though the port does not exist when the kernel parses the line: the
# console is registered later, as soon as the initramfs creates the function,
# and output resumes. Nothing before that point is recoverable either way.
#
# Usage:
#   pmos-usb-console.sh <boot.img> [out.img] [our-Image]
#
# SPDX-License-Identifier: GPL-2.0-only

set -e

BOOTIMG="${1:?нужен путь к boot.img}"
OUTIMG="${2:-${BOOTIMG%.img}-usb.img}"
OUR_IMAGE="${3:-$HOME/dm-kernel/out/arch/arm64/boot/Image}"
MAGISKBOOT="${MAGISKBOOT:-$HOME/ak3-darkmoon/tools/magiskboot}"
CONSOLE="ttyGS0,115200n8"

say() { printf '%s\n' "$*"; }
die() { printf 'ошибка: %s\n' "$*" >&2; exit 1; }

[ -f "$BOOTIMG" ] || die "нет $BOOTIMG"
[ -f "$MAGISKBOOT" ] || die "нет magiskboot"

# magiskboot in ak3-darkmoon is a 32-bit ARM binary; binfmt_misc only covers
# armeb on x86_64, so without this it fails with "Exec format error".
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
[ -f "$WORK/header" ] || die "нет файла header - распаковка без -h"

say "=== распаковываю initramfs"
RD="$WORK/rd"
mkdir -p "$RD"
( cd "$RD" && cpio -idm --quiet < "$WORK/ramdisk.cpio" ) 2>/dev/null ||
	die "cpio не распаковал ramdisk"
[ -f "$RD/init_functions.sh" ] || die "в initramfs нет init_functions.sh"

say "=== проверяю, что gadget действительно поднимает pmOS"
if ! grep -q 'CONFIGFS/g1/functions/"$usb_network_function"' "$RD/init_functions.sh"; then
	die "не нашёл создание функции сети в setup_usb_network_configfs - версия initramfs другая"
fi

# Insert the ACM function right after the network function is linked into the
# configuration, and before the UDC is bound. Binding is what activates the
# gadget, so the link has to exist first.
python3 - "$RD/init_functions.sh" "$CONSOLE" <<'PY'
import sys

path, console = sys.argv[1], sys.argv[2]
src = open(path, encoding='utf-8').read()

ANCHOR = '''	# Link the network instance to the configuration
	ln -s $CONFIGFS/g1/functions/"$usb_network_function" $CONFIGFS/g1/configs/c.1 \\
		|| echo "  Couldn't symlink $usb_network_function"
'''

ADD = '''
	# --- lind: add a serial function, added by tools/pmos-usb-console.sh ---
	#
	# The gadget above gives the device a network function and nothing else,
	# so there is no /dev/ttyGS0 and console=ttyGS0 has no port to write to.
	# ACM is built into our kernel (CONFIG_USB_CONFIGFS_ACM=y), so the
	# function can simply be added here, before the UDC bind below activates
	# the configuration.
	if [ ! -e "$CONFIGFS/g1/functions/acm.GS0" ]; then
		if mkdir "$CONFIGFS/g1/functions/acm.GS0"; then
			ln -s "$CONFIGFS/g1/functions/acm.GS0" \\
				"$CONFIGFS/g1/configs/c.1/acm.usb" ||
				echo "  Couldn't symlink acm.GS0"
			echo "  Added ACM function, console will be /dev/${console%%,*}"
		else
			echo "  Couldn't create ACM function"
		fi
	fi
	# --- end lind ---
'''

if 'lind: add a serial function' in src:
    print('  ACM уже добавлен')
elif ANCHOR in src:
    src = src.replace(ANCHOR, ANCHOR + ADD, 1)
    open(path, 'w', encoding='utf-8').write(src)
    print('  ACM добавлен в setup_usb_network_configfs')
else:
    sys.exit('ОШИБКА: не нашёл якорь для вставки')
PY

say "=== добавляю console=$CONSOLE в cmdline"
if grep -q '^cmdline=' "$WORK/header"; then
	CMD=$(grep '^cmdline=' "$WORK/header" | head -1 | cut -d= -f2-)
	case "$CMD" in
	*console=*)
		sed -i "s|^cmdline=.*|cmdline=$CMD console=$CONSOLE|" "$WORK/header"
		say "  console уже был, добавлен ещё $CONSOLE"
		;;
	*)
		sed -i "s|^cmdline=.*|cmdline=$CMD console=$CONSOLE|" "$WORK/header"
		say "  добавлен console=$CONSOLE"
		;;
	esac
	grep '^cmdline=' "$WORK/header" | sed 's/^/  /'
else
	die "в header нет cmdline"
fi

say "=== собираю initramfs обратно"
# --owner=0:0 matters: cpio run as a normal user would otherwise record every
# member as uid 1000, and the initramfs pmbootstrap built has them all as root.
# Nothing in here is setuid, so it would probably boot either way, but a
# filesystem that belongs to the wrong uid is exactly the sort of thing that
# fails later and far from the cause.
( cd "$RD" && find . | cpio -o -H newc --owner=0:0 --quiet ) < /dev/null > "$WORK/ramdisk.cpio" ||
	die "cpio не собрал initramfs"
say "  ramdisk.cpio: $(stat -c%s "$WORK/ramdisk.cpio") байт"

# repack reads its components from the current directory
( cd "$WORK" && mb repack "$BOOTIMG" "$OUTIMG" ) >/dev/null 2>&1 ||
	die "magiskboot не пересобрал"

say ""
say "=== готово: $OUTIMG ($(stat -c%s "$OUTIMG") байт)"

say ""
say "=== проверка"
V=$(mktemp -d)
trap 'rm -rf "$WORK" "$V"' EXIT
( cd "$V" && mb unpack -h "$OUTIMG" ) >/dev/null 2>&1
VR="$V/rd"
mkdir -p "$VR"
( cd "$VR" && cpio -idm --quiet < "$V/ramdisk.cpio" ) 2>/dev/null

grep -q 'acm.GS0' "$VR/init_functions.sh" &&
	say "  ACM в initramfs: ДА" ||
	{ say "  ACM в initramfs: НЕТ"; exit 1; }
grep -q "console=$CONSOLE" "$V/header" &&
	say "  console=$CONSOLE в cmdline: ДА" ||
	{ say "  console в cmdline: НЕТ"; exit 1; }
[ -f "$V/kernel" ] && say "  ядро на месте: $(stat -c%s "$V/kernel") байт"