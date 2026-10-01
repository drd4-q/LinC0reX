#!/usr/bin/env bash
#
# Deploy the current build to the phone and answer the questions that need a
# screen and a pair of eyes, with as few manual steps as possible.
#
# The phone reboots often enough that hand-assembling a test is a dozen steps and
# any of them can be the stale one, so each case here is one command that also
# gives the panel back to Android afterwards.
#
#   ./deploy.sh deploy    push everything, check the chroot, print the plan
#   ./deploy.sh flip      run the page flip path, report the counters
#   ./deploy.sh scan      run the banded path, report the counters
#
# "report" is the honest word: what a test prints is what the counters say, not
# what the panel looks like. Whether the text is legible and the panel stops
# flickering can only be answered by looking.

set -e

BIN="$HOME/lind"
TEST="$HOME/lindtest"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT="$HERE/lindroid"

PHONE_BIN=/data/lindroid/bin
ROOTFS=/data/lindroid/droot

wait_device() {
	local i=0
	while [ $i -lt 30 ]; do
		if adb devices 2>/dev/null | grep -q "device$"; then
			return 0
		fi
		sleep 5
		i=$((i + 1))
	done
	echo "устройство не появилось" >&2
	return 1
}

root() {
	adb shell su -c "$1"
}

deploy() {
	wait_device

	echo "=== жду, пока Android поднимет панель"
	local i=0
	while [ $i -lt 30 ]; do
		if [ "$(root 'getprop init.svc.surfaceflinger' 2>/dev/null | tr -d '\r')" = running ]; then
			break
		fi
		sleep 5
		i=$((i + 1))
	done

	echo "=== кладу бинарники"
	adb push "$BIN" "$ROOTFS/usr/bin/lind" >/dev/null
	adb push "$TEST" "$ROOTFS/usr/bin/lindtest" >/dev/null
	adb push "$SCRIPT" "$PHONE_BIN/lindroid" >/dev/null
	adb push "$SCRIPT" /sdcard/Download/lindroid >/dev/null
	root "chmod 755 $PHONE_BIN/lindroid"

	echo "=== проверяю chroot"
	root "
		echo -n 'lind:      '; ls -l $ROOTFS/usr/bin/lind      2>&1 | awk '{print \$5}'
		echo -n 'lindtest:  '; ls -l $ROOTFS/usr/bin/lindtest  2>&1 | awk '{print \$5}'
		echo -n 'freetype:  '; ls $ROOTFS/usr/lib/aarch64-linux-gnu/libfreetype.so.6 >/dev/null 2>&1 && echo есть || echo НЕТ
		echo -n 'шрифт:     '; ls $ROOTFS/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf >/dev/null 2>&1 && echo есть || echo НЕТ
		echo -n 'панель:    '; ls /dev/dri/card0 >/dev/null 2>&1 && echo есть || echo НЕТ
	"

	cat <<'PLAN'

=== что смотреть дальше

  $PHONE_BIN/lindroid demo 40        полосный путь отключён, обычный флип
  $PHONE_BIN/lindroid scansync 40    полосная отрисовка по ходу луча

После каждого - счётчики:

  adb shell su -c "grep -a commits /data/lindroid/lind.log | tail -1"

Что означают числа:
  vblank=NNN   события DRM_EVENT_CRTC_SEQUENCE приходят. Ноль или не растёт -
               вся схема полос держится на константе, чинить это первым.
  scan=NNN     кадров, нарисованных полосами. Должно расти вместе с vblank.
  frames=NNN   показано клиенту. Ноль при непустом scan - клиент не получает
               frame callback.
  events=NNN   завершений флипа. В полосном режиме их быть не должно.
  flip_err     ошибок флипа. Растёт - что-то не так с буферами.

Панель Android после любого запуска: $PHONE_BIN/lindroid back
PLAN
}

run_case() {
	local mode=$1

	wait_device
	echo "=== $mode: останавливаю всё, что могло остаться от прошлого раза"
	root "pkill -9 lind 2>/dev/null; pkill -9 lindtest 2>/dev/null; sleep 1" || true
	root "$PHONE_BIN/lindroid back >/dev/null 2>&1" || true

	echo "=== $mode: запуск на 40 секунд"
	root "$PHONE_BIN/lindroid $mode 40" || true

	sleep 8
	echo "=== счётчики"
	root "grep -a 'commits' /data/lindroid/lind.log | tail -1" || true
	root "grep -a 'vblank' /data/lindroid/lind.log | tail -3" || true
	echo "=== ошибки, если есть"
	root "grep -aiE 'ошиб|не удал|failed|QUEUE_SEQUENCE' /data/lindroid/lind.log | head -5" || true
	echo "=== клиент"
	root "tail -2 /data/lindroid/lindtest.log" || true

	echo "=== возвращаю панель Android"
	root "pkill -9 lindtest 2>/dev/null; pkill -9 lind 2>/dev/null; sleep 2" || true
	root "$PHONE_BIN/lindroid back >/dev/null 2>&1" || true
	root "setprop ctl.start surfaceflinger; setprop ctl.start vendor.qti.hardware.display.composer" || true
	sleep 5
	root "echo SF=\$(getprop init.svc.surfaceflinger) composer=\$(getprop init.svc.vendor.qti.hardware.display.composer)"
}

case "${1:-deploy}" in
deploy) deploy ;;
flip)   run_case demo ;;
scan)   run_case scansync ;;
*)      echo "usage: $0 [deploy|flip|scan]" >&2; exit 2 ;;
esac