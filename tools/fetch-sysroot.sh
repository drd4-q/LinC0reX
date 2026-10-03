#!/usr/bin/env bash
#
# Fetch just enough of the chroot to cross-compile, from the phone.
#
# /tmp gets wiped on this machine, and with it the copy of the chroot that
# cross-compiling uses as its sysroot. Rebuilding the whole thing is 435 MB for
# what is actually a few megabytes of headers and libraries, so this pulls the
# build inputs and nothing else.
#
# It matters that these are the phone's own copies. lind was bitten once by
# toolchain skew: built against newer headers it linked GLIBC 2.38 while the
# chroot has 2.36, and the failure looked like a missing interpreter rather than
# a version mismatch.
#
# One adb round trip: the phone tars the whole set, we unpack it. Per-file
# fetching is a hundred round trips and minutes.
#
#   ./fetch-sysroot.sh [phone] [dest]
#
# Defaults: phone 6877e8321022, dest /tmp/opencode/droot

set -e

PHONE="${1:-6877e8321022}"
DEST="${2:-/tmp/opencode/droot}"
REMOTE=/data/lindroid/droot

say() { printf '%s\n' "$*"; }

adb -s "$PHONE" get-state >/dev/null 2>&1 || {
	say "телефон $PHONE недоступен"
	exit 1
}

# adb already runs as root in the ksu domain, so no su wrapper. At one point su
# was not present on the phone at all and this still worked.
[ "$(adb -s "$PHONE" shell id -u 2>/dev/null | tr -d '\r')" = "0" ] || {
	say "adb не от root"
	exit 1
}

LIBS="libwayland-server.so libwayland-client.so libwayland.so \
libwayland-egl.so libwayland-cursor.so libdrm.so libfreetype.so \
libpng16.so libexpat.so libffi.so libz.so libbrotlicommon.so libbrotlidec.so \
libwayland-egl.so"

# Build the list of paths to fetch: header directories, the loose headers those
# include by bare name, and every file matching each library name.
paths=""
for d in freetype2 libdrm wayland-server wayland-client KHR egl glesv2; do
	paths="$paths usr/include/$d"
done

loose=$(adb -s "$PHONE" shell "ls $REMOTE/usr/include/*.h 2>/dev/null" |
	tr -d '\r' | tr '\n' ' ')
paths="$paths $loose"

for l in $LIBS; do
	found=$(adb -s "$PHONE" shell \
		"cd $REMOTE/usr/lib/aarch64-linux-gnu && ls ${l} ${l}.* 2>/dev/null" |
		tr -d '\r' | tr '\n' ' ')
	for f in $found; do
		paths="$paths usr/lib/aarch64-linux-gnu/$f"
	done
done

# Only what exists; tar fails on a single missing path and takes everything
# with it.
ok=""
for p in $paths; do
	if adb -s "$PHONE" shell "[ -e $REMOTE/$p ] && echo yes" 2>/dev/null |
		grep -q yes; then
		ok="$ok $p"
	fi
done

say "=== тяну $(echo $ok | wc -w) путей"
rm -rf "$DEST"
mkdir -p "$DEST"
# exec-out, not shell: adb shell mangles a binary stream - it translates line
# endings - and a tar archive unpacked from that is silently corrupt. Headers
# survive it, which is why this looked like "some paths arrived" rather than
# "the transfer is broken".
adb -s "$PHONE" exec-out "cd $REMOTE && tar cf - $ok" | tar xf - -C "$DEST"

say "=== готово"
check() {
	if [ -e "$DEST/$2" ]; then say "  $1: есть"; else say "  $1: НЕТ"; fi
}
check freetype2 usr/include/freetype2/ft2build.h
check wayland-util.h usr/include/wayland-util.h
check libdrm usr/include/libdrm/drm.h
check libwayland-server usr/lib/aarch64-linux-gnu/libwayland-server.so
check libfreetype usr/lib/aarch64-linux-gnu/libfreetype.so
say "  всё в $DEST"