#!/usr/bin/env bash
#
# pmos-lind-kernel.sh - put our SDE kernel into a pmbootstrap boot image.
#
# pmbootstrap builds a boot.img with a mainline kernel and the mainline dtb
# named in deviceinfo (sm6375-xiaomi-moonstone-2), expecting SimpleFB for the
# 1080x2400 panel. SimpleFB is a linear framebuffer console, not a panel driver:
# mainline has no driver for this display at all, because it is SDE, which is
# vendor-only. So the image pmbootstrap produces boots, and shows a console
# that nobody can reach because the panel is never programmed.
#
# The fix is the kernel, not the userland. Ours is the only one that can drive
# the panel. So:
#
#   1. unpack the pmOS boot.img
#   2. replace the kernel with ours
#   3. drop the mainline dtb, if pmbootstrap put one there: the panel tree is
#      not ours to supply, and a second dtb is what the mainline one would have
#      been used for
#   4. fix the command line - pmOS's asks for SimpleFB and has no earlycon
#      that lands on a UART we have
#   5. repack with the header parameters pmbootstrap used, so the result is
#      byte-compatible with what the device expects
#
# Verified against this device: header v3, os_version 17.0.0, os_patch_level
# 2026-08, page size 4096 - all matching the stock boot.img, which is what
# deviceinfo_generate_bootimg already declares.
#
# Our Image carries no device tree of its own. See docs/pmos-dtb.md: it has zero
# FDT blobs, and in out/System.map __dtb_start equals __dtb_end, so the builtin
# dtb is empty. The panel tree comes from outside boot.img, via the bootloader.
# Anything that claims our Image embeds the vendor dtb is wrong.
#
# Usage:
#   pmos-lind-kernel.sh <pmos-boot.img> [our-Image] [out.img]
#
# SPDX-License-Identifier: GPL-2.0-only

set -e

BOOTIMG="${1:?нужен путь к boot.img от pmbootstrap}"
OUR_IMAGE="${2:-$HOME/dm-kernel/out/arch/arm64/boot/Image}"
OUT="${3:-${BOOTIMG%.img}-lind.img}"

MAGISKBOOT="${MAGISKBOOT:-$HOME/ak3-darkmoon/tools/magiskboot}"

# magiskboot in ak3-darkmoon is a 32-bit ARM binary, so on x86_64 it only runs
# under qemu-arm. binfmt_misc is not registered for little-endian arm here
# (only armeb), so the failure is an immediate "Exec format error" that looks
# like a broken binary rather than a missing emulator.
#
# The architecture is read from the ELF header rather than by trying to run it:
# asking the binary whether it works is unreliable, because magiskboot prints
# its usage and exits non-zero when given no usable arguments.
mb() {
	case "$(file -b "$MAGISKBOOT")" in
	*ARM*)
		command -v qemu-arm >/dev/null 2>&1 ||
			die "magiskboot собран под ARM, а qemu-arm не найден"
		qemu-arm "$MAGISKBOOT" "$@"
		;;
	*)
		"$MAGISKBOOT" "$@"
		;;
	esac
}

say() { printf '%s\n' "$*"; }
die() { printf 'ошибка: %s\n' "$*" >&2; exit 1; }

[ -f "$BOOTIMG" ] || die "нет $BOOTIMG"
[ -f "$OUR_IMAGE" ] || die "нет нашего ядра $OUR_IMAGE"
[ -f "$MAGISKBOOT" ] || die "нет magiskboot по пути $MAGISKBOOT"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# magiskboot unpack takes no output directory: it writes into the current
# directory, and only writes the header file when -h is given. Both are easy to
# get wrong and magiskboot reports neither - it unpacks happily into the wrong
# place and the header simply is not there.
say "=== исходный boot.img"
( cd "$WORK" && mb unpack -h "$BOOTIMG" ) >/dev/null 2>&1 ||
	die "magiskboot не распаковал $BOOTIMG"

[ -f "$WORK/kernel" ] || die "после распаковки нет kernel - образ не тот"

for f in header kernel ramdisk.cpio second dtb; do
	[ -f "$WORK/$f" ] && say "  $f: $(stat -c%s "$WORK/$f") байт"
done
[ -f "$WORK/header" ] || die "нет файла header - распаковка без -h"

# What pmOS thinks the console is. Kept for the record: it is why the built
# image shows nothing on this panel even when it boots.
say "=== параметры заголовка, которые сохраняем"
grep -E '^(header_version|os_version|os_patch_level|page_size|cmdline)=' \
	"$WORK/header" | sed 's/^/  /' || true

cp "$OUR_IMAGE" "$WORK/kernel"
say "=== подставлено наше ядро: $(stat -c%s "$WORK/kernel") байт"

# The mainline dtb lives here in the image pmbootstrap makes. Ours is appended
# to the Image already, and leaving a stale dtb in place is how you end up
# debugging a kernel that boots with the wrong device tree.
if [ -f "$WORK/dtb" ]; then
	say "=== удаляю mainline dtb ($(stat -c%s "$WORK/dtb") байт)"
	rm -f "$WORK/dtb"
fi

# console=tty0 with SimpleFB is meaningless for a panel that has to be brought
# up by a driver. earlycon on the real UART is not: that is the serial console
# pmOS is usable over, and it is the only output available before the panel
# driver is alive.
if grep -q '^cmdline=' "$WORK/header"; then
	sed -i 's|^cmdline=.*|cmdline=nokaslr kpti=off noirqdebug earlycon clk_ignore_unused pd_ignore_unused regulator_ignore_unused|' \
		"$WORK/header"
	say "=== cmdline заменён на рабочий для этого ядра"
fi

# repack takes the original image as the source of truth for the header, and
# takes every component it finds in the *current* directory. It has to run
# inside $WORK: called from anywhere else it finds no components, silently
# falls back to the original image, and produces a byte-identical copy that
# still carries the mainline kernel. Nothing warns about this - the output is
# a valid boot image, just the wrong one.
( cd "$WORK" && mb repack "$BOOTIMG" "$OUT" ) >/dev/null 2>&1 ||
	die "magiskboot не пересобрал"

say "=== готово: $OUT ($(stat -c%s "$OUT") байт)"

say ""
say "=== проверка"
unpack_tmp=$(mktemp -d)
trap 'rm -rf "$WORK" "$unpack_tmp"' EXIT
( cd "$unpack_tmp" && mb unpack -h "$OUT" ) >/dev/null 2>&1
if cmp -s "$unpack_tmp/kernel" "$OUR_IMAGE"; then
	say "  ядро совпадает с нашим: ДА"
else
	say "  ядро совпадает с нашим: НЕТ - это ошибка, не прошивай"
	exit 1
fi
[ -f "$unpack_tmp/dtb" ] && say "  dtb на месте: ОСТАЛСЯ, проверь" ||
	say "  dtb отсутствует: да"
say "  размер: $(stat -c%s "$unpack_tmp/kernel") байт ядра, $(stat -c%s "$unpack_tmp/ramdisk.cpio") байт ramdisk"