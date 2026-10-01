#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# gen-protocols.sh - generate the Wayland protocol bindings lind and lindtest use.
#
# The generated files are not in git: they are a mechanical product of an XML
# file and a tool, and carrying them makes every scanner version bump look like
# a real change. Regenerate instead.
#
# Needs the chroot unpacked at $ROOTFS and wayland-protocols plus
# wayland-scanner inside it, since the scanner is an arm64 binary that wants
# the chroot's own libraries.
#
#   tools/gen-protocols.sh [rootfs]
#
# After running, the server binding is compiled into lind and the client
# binding into lindtest. Build both with the sysroot flags in the file
# headers, or the result links against GLIBC 2.38 and the chroot's 2.36
# refuses to load it.

set -e

ROOTFS=${1:-/data/lindroid/droot}
HERE=$(cd "$(dirname "$0")" && pwd)
XML="$ROOTFS/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml"
SCANNER="$ROOTFS/usr/bin/wayland-scanner"

[ -f "$XML" ] || {
	echo "нет $XML" >&2
	echo "поставь wayland-protocols в chroot через fetchdeb.py" >&2
	exit 1
}
[ -x "$SCANNER" ] || {
	echo "нет $SCANNER" >&2
	exit 1
}

# The scanner is an arm64 binary. On the workstation it has to run under
# qemu-user with the chroot as the library prefix, and LD_LIBRARY_PREFIX set,
# because the kernel's binfmt only resolves the interpreter for the binary
# itself, not for what it loads afterwards.
run_scanner() {
	if [ "$(uname -m)" = "aarch64" ]; then
		LD_LIBRARY_PATH="$ROOTFS/usr/lib/aarch64-linux-gnu" \
			chroot "$ROOTFS" /usr/bin/wayland-scanner "$@"
	else
		command -v qemu-aarch64 >/dev/null || {
			echo "нужен qemu-aarch64 для генерации" >&2
			exit 1
		}
		# shellcheck disable=SC2034
		QEMU_LD_PREFIX="$ROOTFS" \
		LD_LIBRARY_PATH="$ROOTFS/usr/lib/aarch64-linux-gnu" \
			qemu-aarch64 "$SCANNER" "$@"
	fi
}

echo "генерирую из $XML"
run_scanner server-header "$XML" "$HERE/xdg-shell-server-protocol.h"
run_scanner client-header "$XML" "$HERE/xdg-shell-client-protocol.h"
run_scanner private-code  "$XML" "$HERE/xdg-shell-protocol.c"

ls -l "$HERE"/xdg-shell-*protocol* "$HERE"/xdg-shell-protocol.c
echo "готово"
