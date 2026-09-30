#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# panel.sh - hand the display between Android and Linux on this phone.
#
# The steps this wraps up were all found the hard way, and none of them are
# guessable from the error messages. See docs/panel-handover.md.
#
#   panel.sh take     stop the display owners and let Linux have the panel
#   panel.sh release  give the panel back to Android
#   panel.sh status   who holds card0 right now
#
# "take" does not itself put anything on screen. It stops the two services that
# hold the display and then starts kmsclaim, which takes DRM master and scans
# out a colour-bar test image. Use tools/fetchdeb.py to build the chroot first;
# this script assumes /data/lindroid/droot is already there.
#
# Everything here needs root. Run it under `su -c` on the device.

ROOTFS=/data/lindroid/droot
CARD=/dev/dri/card0

# The two services that hold the panel. SurfaceFlinger is the obvious one, but
# the Qualcomm composer HAL opens card0 by itself and is what actually has to
# be stopped - with only SurfaceFlinger down, composer-servic keeps six
# descriptors on the card and nothing else can take master.
SF=surfaceflinger
COMPOSER=vendor.qti.hardware.display.composer

say() { echo "== $*"; }
err() { echo "!! $*" >&2; }

svc_state() { getprop "init.svc.$1"; }

holders() {
	# Which processes have card0 open right now.
	for p in /proc/[0-9]*; do
		[ -d "$p/fd" ] || continue
		for f in "$p"/fd/*; do
			t=$(readlink "$f" 2>/dev/null) || continue
			case "$t" in
			*card0*) echo "$(cat "$p/comm" 2>/dev/null)" ;;
			esac
		done
	done | sort -u
}

status() {
	say "services"
	printf '  %-46s %s\n' "$SF" "$(svc_state "$SF")"
	printf '  %-46s %s\n' "$COMPOSER" "$(svc_state "$COMPOSER")"

	say "panel"
	if [ -e /sys/class/drm/card0-DSI-1/enabled ]; then
		printf '  %-46s %s\n' "card0-DSI-1/status" \
			"$(cat /sys/class/drm/card0-DSI-1/status)"
		printf '  %-46s %s\n' "card0-DSI-1/enabled" \
			"$(cat /sys/class/drm/card0-DSI-1/enabled)"
	else
		echo "  no DSI connector on card0"
	fi

	say "processes holding $CARD"
	h=$(holders)
	if [ -z "$h" ]; then
		echo "  (none)"
	else
		echo "$h" | sed 's/^/  /'
	fi
}

take() {
	[ "$(id -u)" = 0 ] || { err "run as root"; exit 1; }

	say "stopping the display owners"
	setprop "ctl.stop $SF"
	setprop "ctl.stop $COMPOSER"

	# init reaps the services asynchronously; wait for the fds to go away
	# rather than sleeping a fixed amount.
	i=0
	while [ $i -lt 20 ]; do
		[ -z "$(holders)" ] && break
		sleep 1
		i=$((i + 1))
	done
	if [ -n "$(holders)" ]; then
		err "something still holds $CARD after ${i}s:"
		holders | sed 's/^/  /'
		exit 1
	fi
	say "$CARD is free"

	[ -d "$ROOTFS" ] || { err "no chroot at $ROOTFS"; exit 1; }

	# kmsclaim stays in the foreground on purpose: DRM master is released when
	# its fd closes, so the process being alive is what keeps the panel ours.
	say "starting kmsclaim (Ctrl-C or 'panel.sh release' to hand back)"
	exec chroot "$ROOTFS" /usr/bin/kmsclaim
}

release() {
	[ "$(id -u)" = 0 ] || { err "run as root"; exit 1; }

	say "stopping kmsclaim"
	pkill -f kmsclaim 2>/dev/null
	sleep 1

	# Composer first, SurfaceFlinger second. The other order leaves
	# SurfaceFlinger starting against a display nothing owns.
	say "restarting the display owners"
	setprop "ctl.start $COMPOSER"
	sleep 1
	setprop "ctl.start $SF"

	sleep 2
	status
}

case "${1:-status}" in
take)    take ;;
release) release ;;
status)  status ;;
*)
	cat >&2 <<-EOF
	usage: $0 {take|release|status}

	  take      stop the display owners, give the panel to Linux
	  release   give the panel back to Android
	  status    show who holds the panel right now
	EOF
	exit 2
	;;
esac
