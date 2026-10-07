#!/usr/bin/env bash
#
# pmos-pmos-initramfs.sh - patch the pmOS initramfs so it can boot on this
# phone, then build a boot image from it.
#
# The pmOS initramfs works everywhere else, so the changes are deliberately
# minimal and additive. Each one exists because a specific failure was measured
# on this device; docs/pmos-boot-failure.md has the full story.
#
# Patch 1 - device symlinks and the hotplug handler.
#
#   `# CONFIG_DEVTMPFS is not set` and CONFIG_UEVENT_FS are unset, so nothing
#   publishes device events and nothing creates /dev/block/by-name or
#   /dev/disk/by-partlabel. mount_subpartitions() looks only at
#   /dev/disk/by-partlabel/userdata, /dev/block/sde19 and /dev/mapper/system* -
#   all empty. So the partitions inside userdata are never created and
#   pmOS_root is never found. Everything needed is already in blkid's output.
#
# Patch 2 - node creation after losetup -P.
#
#   Even with the symlinks, `losetup -P` leaves the kernel with loop0p1 and
#   loop0p2 in /proc/partitions and no /dev nodes for them, because the mdev
#   pass ran earlier. blkid probes nodes only, so it finds no pmOS_root. Re-run
#   mdev afterwards and mknod whatever is still missing, taking major and minor
#   from /proc/partitions.
#
# Patch 3 - an ACM gadget function.
#
#   There is no console on this device: CONFIG_FRAMEBUFFER_CONSOLE, CONFIG_VT
#   and CONFIG_DRM_FBDEV_EMULATION are all unset, so there is no tty0, and the
#   stock cmdline is empty so /dev/console goes nowhere. pmOS's init already
#   builds a gadget with an NCM network function; adding ACM gives /dev/ttyGS0
#   and with it the only way to see anything. It has to be added before the UDC
#   bind, which is what activates the configuration.
#
# The boot image is then built from the STOCK boot.img, not from the pmOS one.
# See the header comment of pmos-initramfs.sh: this bootloader only boots
# header_version 3, and both images that work here are magiskboot repacks of
# stock. pmOS documents boot.img as "header <=2", which is exactly why the
# pmbootstrap-built image is refused.
#
# Usage:
#   pmos-pmos-initramfs.sh [out.img]
#
# SPDX-License-Identifier: GPL-2.0-only

set -e

OUTIMG="${1:-$HOME/lindroid-kernel/out/boot-pmos-final.img}"
STOCK="$HOME/kernel_xiaomi_stone/stock/boot.img"
OUR_IMAGE="$HOME/ak3-darkmoon/Image"
PMOS_INITRAMFS="$HOME/pmos_kernel/chroot_rootfs_xiaomi-moonstone/boot/initramfs"
MAGISKBOOT="${MAGISKBOOT:-$HOME/ak3-darkmoon/tools/magiskboot}"

say() { printf '%s\n' "$*"; }
die() { printf 'ошибка: %s\n' "$*" >&2; exit 1; }

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

RD="$WORK/rd"
mkdir -p "$RD"
say "=== распаковываю pmOS initramfs"
( cd "$RD" && gzip -dc "$PMOS_INITRAMFS" | cpio -idm --quiet ) 2>/dev/null ||
	die "initramfs не распаковался"
[ -x "$RD/usr/bin/busybox" ] || die "нет busybox"

say "=== патчу init_functions.sh и init"
python3 - "$RD" <<'PY'
import re
import sys
import os

rd = sys.argv[1]
fn = os.path.join(rd, 'init_functions.sh')
init = os.path.join(rd, 'init')
fs = open(fn, encoding='utf-8').read()
it = open(init, encoding='utf-8').read()

# ---------------------------------------------------------------- patch 1 + 2
FUNCS = '''
# --- lind: правки для телефона без udev ---------------------------------
#
# 1) Симлинки /dev/block/by-name и /dev/disk/by-partlabel. Их создаёт udev,
#    которого в initramfs нет: CONFIG_DEVTMPFS не задан, CONFIG_UEVENT_FS тоже.
#    Без них mount_subpartitions() не находит userdata, а установщик не находит
#    раздел для boot.img.
# 2) hotplug-обработчик. Без него ядро не сообщает о новых устройствах, и
#    ни /dev/ttyGS0, ни узлы loop-подразделов не появляются.
# 3) Узлы loop-подразделов после losetup -P: ядро их создаёт, а mdev уже
#    отработал, поэтому имён нет, и blkid не может опросить pmOS_root.
lind_devsymlinks() {
	local dev rest pl pu
	echo /bin/mdev > /proc/sys/kernel/hotplug 2>/dev/null
	mdev -s 2>/dev/null
	mkdir -p /dev/block/by-name /dev/disk/by-partlabel /dev/disk/by-partuuid
	blkid 2>/dev/null | while read -r dev rest; do
		[ -b "$dev" ] || continue
		pl=$(echo "$rest" | sed -n 's/.*PARTLABEL="\\([^"]*\\)".*/\\1/p')
		pu=$(echo "$rest" | sed -n 's/.*PARTUUID="\\([^"]*\\)".*/\\1/p')
		[ -n "$pl" ] && ln -sf "$dev" "/dev/block/by-name/$pl" 2>/dev/null
		[ -n "$pl" ] && ln -sf "$dev" "/dev/disk/by-partlabel/$pl" 2>/dev/null
		[ -n "$pu" ] && ln -sf "$dev" "/dev/disk/by-partuuid/$pu" 2>/dev/null
	done
	# mdev зовёт этот хук на каждый узел; без него сыплется ошибка в вывод
	mkdir -p /lib/mdev && : > /lib/mdev/persistent-storage
	return 0
}

lind_loopnodes() {
	local p maj min
	mdev -s 2>/dev/null
	for p in $(grep -E "loop[0-9]+p[0-9]+" /proc/partitions 2>/dev/null | awk '{print $4}'); do
		maj=$(awk -v p="$p" '$4==p {print $1}' /proc/partitions 2>/dev/null)
		min=$(awk -v p="$p" '$4==p {print $2}' /proc/partitions 2>/dev/null)
		if [ -n "$maj" ] && [ ! -b "/dev/$p" ]; then
			mknod "/dev/$p" b "$maj" "$min" 2>/dev/null
		fi
	done
	mdev -s 2>/dev/null
	return 0
}

# lind_emit - построчный вывод в /dev/ttyGS0.
#
# У этого телефона нет консоли: CONFIG_FRAMEBUFFER_CONSOLE, CONFIG_VT и
# CONFIG_DRM_FBDEV_EMULATION не заданы, так что tty0 не существует, а cmdline
# пустой - значит /dev/console никуда не ведёт. Единственный способ увидеть
# что происходит - писать прямо в порт.
#
# Строка за строкой с паузой, и без перенаправления: проверено, что
# `exec >/dev/ttyGS0` с обычными командами роняет телефон - он перезагружается
# примерно через три секунды после появления gadget. Две записи по байту
# проходят нормально, и двенадцать килобайт построчно тоже.

# lind_ttygs0 - гарантировать, что /dev/ttyGS0 существует.
#
# Может не существовать по двум причинам, и обе проверены на телефоне.
# Первая: ядро не сообщает о новом устройстве, потому что CONFIG_UEVENT_FS не
# задан и работает только вызов программы из /proc/sys/kernel/hotplug - то
# есть нужен mdev. Вторая, даже если узлы и создаются: major у ttyGS
# динамический, и в /proc/devices он появляется вместе с устройством.
lind_ttygs0() {
	local maj
	[ -e /dev/ttyGS0 ] && return 0
	mdev -s 2>/dev/null
	[ -e /dev/ttyGS0 ] && return 0
	maj=$(awk '/ttyGS/{print $1; exit}' /proc/devices 2>/dev/null)
	if [ -n "$maj" ]; then
		mknod /dev/ttyGS0 c "$maj" 0 2>/dev/null &&
			echo "lind: создан /dev/ttyGS0 c $maj 0" > /tmp/tty.log
	fi
	[ -e /dev/ttyGS0 ]
}

lind_emit() {
	lind_ttygs0 || return 0
	while IFS= read -r l; do
		printf '%s\n' "$l" > /dev/ttyGS0 2>/dev/null
		sleep 0.12
	done < "$1"
}

lind_report() {
	{
		echo "=== $(uname -r) ==="
		echo "--- ttyGS0 ---"; ls -l /dev/ttyGS0 2>&1; cat /tmp/tty.log 2>/dev/null
		echo "--- by-name (12) ---"; ls /dev/block/by-name/ 2>/dev/null | head -12
		echo "--- partlabel (12) ---"; ls /dev/disk/by-partlabel/ 2>/dev/null | head -12
		echo "--- sda19 ---"; blkid /dev/sda19 2>&1
		echo "--- loop parts ---"; ls /dev/loop* 2>&1 | head -20
		echo "--- pmOS в blkid ---"; blkid 2>/dev/null | grep -i pmOS
		echo "--- PMOS_ROOT=[$PMOS_ROOT] PMOS_BOOT=[$PMOS_BOOT] ---"
		echo "--- partitions ---"; cat /proc/partitions 2>/dev/null | grep -E "loop|sda19"
		echo "--- dmesg хвост ---"; dmesg 2>/dev/null | tail -20
	} > /tmp/r1.txt 2>&1
	lind_emit /tmp/r1.txt
}
# --- end lind ---------------------------------------------------------

'''

if 'lind_devsymlinks' not in fs:
    anchor = '\nmount_proc_sys_dev() {'
    if anchor not in fs:
        sys.exit('ОШИБКА: не нашёл mount_proc_sys_dev в init_functions.sh')
    fs = fs.replace(anchor, FUNCS + anchor.lstrip('\n'), 1)
else:
    print('  функции уже есть')

# ---------------------------------------------------------------- patch 3
ACM = '''
	# --- lind: серийная функция, чтобы был /dev/ttyGS0 ---
	# У этого ядра нет framebuffer-консоли (CONFIG_FRAMEBUFFER_CONSOLE,
	# CONFIG_VT и CONFIG_DRM_FBDEV_EMULATION не заданы), а cmdline пустой, так
	# что /dev/console никуда не ведёт. Без ACM не видно ничего. Добавляем до
	# привязки UDC - именно она включает конфигурацию.
	if [ ! -e "$CONFIGFS/g1/functions/acm.GS0" ]; then
		if mkdir "$CONFIGFS/g1/functions/acm.GS0" 2>/dev/null; then
			ln -s "$CONFIGFS/g1/functions/acm.GS0" \
				"$CONFIGFS/g1/configs/c.1/acm.usb" 2>/dev/null ||
				echo "  Couldn't symlink acm.GS0"
		fi
	fi
	# --- end lind ---
'''

LINK_ANCHOR = '''	ln -s $CONFIGFS/g1/functions/"$usb_network_function" $CONFIGFS/g1/configs/c.1 \\
		|| echo "  Couldn't symlink $usb_network_function"
'''
if 'lind: серийная функция' not in fs:
    if LINK_ANCHOR not in fs:
        sys.exit('ОШИБКА: не нашёл привязку сетевой функции в setup_usb_network_configfs')
    fs = fs.replace(LINK_ANCHOR, LINK_ANCHOR + ACM, 1)
else:
    print('  ACM уже есть')

# ------------------------------------------------------- patch 4: узлы loop
# mount_subpartitions() после losetup -Pf проверяет, тот ли это раздел:
#
#     SUBPARTITION_LOOP="$(losetup $losetup_args "$partition")"
#     ...
#     find_root_partition
#     if [ -n "$PMOS_ROOT" ]; then break; fi
#
# find_root_partition ищет метку pmOS_root через blkid, а blkid обходит /dev.
# Ядро разделы loop видит (/sys/block/loop1/loop1p1 есть), но узлов в /dev не
# создаёт: CONFIG_DEVTMPFS не задан, поэтому узлы заводятся только mdev, а он
# к этому моменту отработал давно. Итог - PMOS_ROOT пуст, цикл отвязывает loop
# и повторяет попытку, пока не выйдет десятисекундный лимит.
#
# Вызов стоял в init ПОСЛЕ mount_subpartitions, то есть ровно тогда, когда узлы
# уже никому не нужны. Переносим его внутрь функции, до find_root_partition.
# Определения lind_* лежат в этом же файле, поэтому ищем именно ВЫЗОВ -
# голое имя на отдельной строке, без скобок.
CALL = re.compile(r'[ \t]lind_loopnodes[ \t]*\n')
if not CALL.search(fs):
    m = re.search(r'([ \t]*)# Ensure that this was the \*correct\* subpartition', fs)
    if not m:
        sys.exit('ОШИБКА: не нашёл "# Ensure that this was the *correct* '
                 'subpartition" в mount_subpartitions')
    fs = fs[:m.start()] + m.group(1) + 'lind_loopnodes\n' + fs[m.start():]
    print('  lind_loopnodes перенесён внутрь mount_subpartitions')
else:
    print('  узлы loop уже создаются внутри mount_subpartitions')

# ------------------------------------------------------- patch 5: direct-io
# mount_subpartitions() вешает подразделы userdata на loop вот так:
#
#     local losetup_args="--show -Pf --direct-io=on"
#     SUBPARTITION_LOOP="$(losetup $losetup_args "$partition")"
#
# На этом телефоне --direct-io=on ломает монтирование. Подложка, userdata
# (sda19), имеет логический сектор 4096 байт, а loop создаётся с блоком 512.
# Прямой ввод-вывод уходит в очередь подложки без разбиения, и запрос ext4 на
# 512 байт отклоняется:
#
#     blk_update_request: I/O error, dev loop0, sector 499714 op READ
#     EXT4-fs (loop0p2): unable to read superblock
#     mount: mounting /dev/loop0p2 on /sysroot failed: Invalid argument
#
# Суперблок при этом на месте - dd bs=1 skip=1080 отдаёт 53 ef, то есть
# 0xEF53, и blkid читает обе метки. Ломается именно чтение из ядра, а не
# разметка. Проверено вживую: losetup -P без --direct-io, потом
# mount - и mount rc=0, в точке монтирования лежит настоящий корень pmOS.
#
# Прямой ввод-вывод здесь - только оптимизация, к корректности отношения не
# имеет, поэтому убираем флаг, а не подгоняем размер сектора.
if '--direct-io=on' in fs:
    fs = fs.replace('--direct-io=on', '', 1)
    print('  --direct-io=on убран из losetup_args')
else:
    print('  --direct-io=on уже отсутствует')

# ------------------------------------------ patch 6: принудительный шелл
# jump_init_2nd() уводит init в stage 2:
#
#     echo "  ❬❬ PMOS STAGE 2 ❭❭"
#     exec /init_2nd.sh
#
# После этого единственный способ попасть внутрь - пароль root, а его у нас
# нет. Отладочный шелл initramfs пароля не требует: это просто /bin/sh от root
# в tmpfs, и getty на ttyGS0 поднимается сам через debug_shell.
#
# Штатный способ - параметр pmos.debug-shell в cmdline, но cmdline приходит
# от загрузчика и пустой, init его сам не формирует. Поэтому подменяем exec
# на debug_shell. Дальше init не должен падать: если он завершится, ядро
# убьётся с "Attempted to kill init", поэтому держим его живым бесконечным
# sleep - getty всё это время работает на фоне.
if os.environ.get('LIND_DEBUG_SHELL'):
    a = 'exec /init_2nd.sh'
    if a in fs:
        fs = fs.replace(a, 'fail_halt_boot  # lind: принудительно, см. LIND_DEBUG_SHELL', 1)
        print('  init остановлен на отладочном шелле (LIND_DEBUG_SHELL)')
    else:
        sys.exit('ОШБОРКА: не нашёл "exec /init_2nd.sh" в jump_init_2nd')

    # debug_shell перед стартом getty блокируется на чтении ttyGS0 - ждёт
    # первый символ от PC, чтобы тот увидел вывод при открытии порта. Если
    # символ не придёт, getty не запустится никогда. Убираем блокировку:
    # echo остаётся (он нужен как признак открытия порта), чтение - нет.
    b = 'read -r < /dev/ttyGS0'
    if b in fs:
        fs = fs.replace(b, ': # lind: не блокируемся на чтении ttyGS0', 1)
        print('  блокирующее чтение ttyGS0 убрано')
    else:
        print('  блокирующего чтения ttyGS0 не нашёл')
else:
    print('  обычная загрузка в stage 2 (LIND_DEBUG_SHELL не задан)')

open(fn, 'w', encoding='utf-8').write(fs)

# ---------------------------------------------------------------- вызовы
if 'lind_devsymlinks' not in it:
    a = '\nmount_proc_sys_dev\n'
    if a not in it:
        sys.exit('ОШИБКА: не нашёл mount_proc_sys_dev в init')
    it = it.replace(a, '\nmount_proc_sys_dev\nlind_devsymlinks\nlind_report\n', 1)

if 'lind_loopnodes' not in it:
    a = '\nmount_subpartitions\n'
    if a not in it:
        sys.exit('ОШИБКА: не нашёл mount_subpartitions в init')
    it = it.replace(a, '\nmount_subpartitions\nlind_loopnodes\nlind_report\n', 1)
    b = '\n# Now we can jump to the 2nd stage init\njump_init_2nd'
    if b in it:
        it = it.replace(b, '\nlind_report\n' + b, 1)

open(init, 'w', encoding='utf-8').write(it)
print('  патчи применены')
PY

say "=== проверяю, что всё на месте"
for f in init init_functions.sh; do
	grep -q lind_devsymlinks "$RD/$f" || die "нет lind_devsymlinks в $f"
	grep -q lind_loopnodes   "$RD/$f" || die "нет lind_loopnodes в $f"
done
grep -q 'acm.GS0' "$RD/init_functions.sh" || die "нет ACM-функции"
grep -q 'direct-io=on' "$RD/init_functions.sh" &&
	die "--direct-io=on остался в losetup_args - userdata с сектором 4096 не смонтируется"
# Узлы loop-разделов обязаны создаваться ДО find_root_partition внутри
# mount_subpartitions. Если вызов остался только в init, после функции, pmOS
# снова уйдёт в отладочную оболочку - ровно то, что уже случалось дважды.
grep -q 'lind_loopnodes' "$RD/init_functions.sh" ||
	die "lind_loopnodes не попал в init_functions.sh - узлы появятся слишком поздно"
python3 - "$RD/init_functions.sh" <<'CHK' || die "lind_loopnodes не перед find_root_partition"
import re, sys
src = open(sys.argv[1], encoding='utf-8').read()
m = re.search(r'[ \t]lind_loopnodes[ \t]*\n', src)
if not m:
    sys.exit('вызова lind_loopnodes нет вовсе')
j = src.find('find_root_partition', m.start())
if j < 0:
    sys.exit('после вызова нет find_root_partition')
if src[m.end():j].count('\n') > 4:
    sys.exit('между вызовом и find_root_partition слишком много текста')
CHK
say "  init: devsymlinks=$(grep -c lind_devsymlinks "$RD/init") loopnodes=$(grep -c lind_loopnodes "$RD/init")"
say "  init_functions.sh: acm.GS0=$(grep -c acm.GS0 "$RD/init_functions.sh") loopnodes=$(grep -c lind_loopnodes "$RD/init_functions.sh")"

say "=== добавляю fdisk с библиотеками"
# mount_subpartitions() считает подразделы вот так:
#
#     part_count="$(fdisk -l "$partition" 2>/dev/null | grep -cE '^ +[0-9]|^'"$partition")"
#     if [ "$part_count" -eq 2 ]; then SUBPARTITION_LOOP="$(losetup --show -Pf ...)"
#
# В initramfs fdisk нет, поэтому part_count всегда 0, до losetup дело не
# доходит, и через десять секунд init уходит в отладочную оболочку вместо
# того, чтобы продолжить загрузку. Комментарий в коде pmOS прямо признаёт,
# что счёт идёт через fdisk "because there doesn't seem to be a better way
# without adding more dependencies to the 1st stage initramfs".
#
# Одного бинарника мало: Alpine разносит util-linux по библиотекам, и без них
# fdisk падает с "Error loading shared library libfdisk.so.1". Набор ниже
# получен разбором DT_NEEDED, рекурсивно - всего 2.1 МБ.
FDISK_ROOT="$HOME/pmos_kernel/chroot_rootfs_xiaomi-moonstone"
FDISK_FILES="
usr/bin/fdisk
lib/libfdisk.so.1
lib/libsmartcols.so.1
lib/libncursesw.so.6
lib/libuuid.so.1
lib/libblkid.so.1
lib/libeconf.so.0
"
for rel in $FDISK_FILES; do
	src="$FDISK_ROOT/$rel"
	[ -f "$src" ] || die "нет $src - без fdisk загрузка уйдёт в отладочную оболочку"
	mkdir -p "$RD/$(dirname "$rel")"
	cp "$src" "$RD/$rel"
	chmod 755 "$RD/$rel"
done
# ld-musl в initramfs уже есть, но версия из того же pmOS base - пусть будет
cp -f "$FDISK_ROOT/lib/libc.musl-aarch64.so.1" "$RD/lib/" 2>/dev/null || true
say "  добавлено файлов: $(echo $FDISK_FILES | wc -w)"

say "=== собираю initramfs обратно"
( cd "$RD" && find . | cpio -o -H newc --owner=0:0 --quiet ) < /dev/null > "$WORK/initramfs.cpio"
# Порядок важен. magiskboot unpack пишет компоненты в текущий каталог, и
# распаковка перезаписывает ramdisk.cpio стоковым. Если положить свой ramdisk
# раньше распаковки, он будет молча заменён, образ соберётся, все проверки
# пройдут - а на телефоне окажется ramdisk от Android. Распаковываем первым.
say "=== распаковываю стоковый boot.img"
( cd "$WORK" && mb unpack -h "$STOCK" ) >/dev/null 2>&1 || die "magiskboot не распаковал сток"

say "=== подставляю компоненты"
cp "$WORK/initramfs.cpio" "$WORK/ramdisk.cpio"
cp "$OUR_IMAGE" "$WORK/kernel"
# cmdline лежит в заголовке образа, и загрузчик берёт его оттуда. У стока он
# пустой, поэтому ядро не получает ни console=, ни pmos.debug-shell, и
# initramfs-шелл недостижим. Здесь его можно задать.
#
# LIND_CMDLINE="pmos.debug-shell" - остановиться в отладочном шелле initramfs,
# где пароль root не нужен (это /bin/sh от root в tmpfs).
# LIND_CMDLINE="console=ttyGS0,115200" - вывод ядра в USB-порт.
CMDLINE="${LIND_CMDLINE-}"
printf 'cmdline=%s\nos_version=16.0.0\nos_patch_level=2026-04\n' "$CMDLINE" > "$WORK/header"
say "  cmdline: ${CMDLINE:-<пусто>}"
( cd "$WORK" && mb repack "$STOCK" "$OUTIMG" ) >/dev/null 2>&1 || die "magiskboot не пересобрал"

say ""
say "=== проверка"
V="$WORK/verify"; mkdir -p "$V"
( cd "$V" && mb unpack -h "$OUTIMG" ) >/dev/null 2>&1
# Проверять ramdisk обязательно: magiskboot молча берёт исходный, если
# компонент назван неверно или если он не на месте на момент repack. Тогда
# образ выглядит собранным правильно, а внутри - ramdisk от Android.
VR="$V/rd"; mkdir -p "$VR"
( cd "$VR" && cpio -idm --quiet < "$V/ramdisk.cpio" ) 2>/dev/null
if [ -L "$VR/init" ]; then
	die "в собранном образе init - симлинк ($(readlink "$VR/init")): это ramdisk от Android, наш не подхватился"
fi
grep -q 'lind_devsymlinks' "$VR/init" ||
	die "в initramfs нет lind_devsymlinks - патчи не применились"
[ -x "$VR/usr/bin/fdisk" ] ||
	die "в initramfs нет fdisk - mount_subpartitions не найдёт подразделы"
for lib in libfdisk.so.1 libsmartcols.so.1 libncursesw.so.6 libuuid.so.1 libblkid.so.1 libeconf.so.0; do
	[ -e "$VR/lib/$lib" ] || die "в initramfs нет lib/$lib - fdisk не запустится"
done
say "  init настоящий, не симлинк"
say "  fdisk: $(stat -c%s "$VR/usr/bin/fdisk") байт, библиотек: 6"
say "  init: devsymlinks=$(grep -c lind_devsymlinks "$VR/init") loopnodes=$(grep -c lind_loopnodes "$VR/init") report=$(grep -c lind_report "$VR/init")"
say "  init_functions.sh: acm.GS0=$(grep -c acm.GS0 "$VR/init_functions.sh") loopnodes=$(grep -c lind_loopnodes "$VR/init_functions.sh")"
python3 - "$V" "$OUR_IMAGE" "$OUTIMG" <<'PY'
import struct, sys
v, ours, img = sys.argv[1:4]
k = open(f'{v}/kernel', 'rb').read()
o = open(ours, 'rb').read()
print(f'  ядро == наш Image: {"ДА" if k == o else "НЕТ"} ({len(k)} байт)')
assert k == o, 'ОШИБКА: ядро не то'
d = open(img, 'rb').read(48)
f = lambda off: int.from_bytes(d[off:off+4], 'little')
print(f'  header_version={f(40)} page_size={f(36)} kernel_size={f(8)}')
assert f(40) == 3, 'ОШИБКА: заголовок не v3, bootloader не возьмёт'
print('  заголовок v3: ДА')
PY

say ""
say "готово: $OUTIMG ($(stat -c%s "$OUTIMG") байт)"