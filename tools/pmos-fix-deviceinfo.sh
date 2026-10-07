#!/bin/sh
# Правка deviceinfo, который читает boot-deploy внутри chroot pmbootstrap.
#
# Запускать от root:  sudo tools/pmos-fix-deviceinfo.sh
#
# ---------------------------------------------------------------------------
# Почему нельзя править pmaports
# ---------------------------------------------------------------------------
# У boot-deploy свой путь к deviceinfo, и он не тот, у который читает pmbootstrap.
# Цепочка такая (boot-deploy-functions.sh, source_deviceinfo):
#
#	1. $local_deviceinfo, если задан флагом -c
#	2. /usr/share/misc/source_deviceinfo, если файл существует
#	3. /usr/share/deviceinfo/deviceinfo, затем /etc/deviceinfo
#
# Второй пункт срабатывает. Этот файл делает так:
#
#	SOURCE_DEVICEINFO_ROOT="${SOURCE_DEVICEINFO_ROOT:-}"
#	[ -f "$SOURCE_DEVICEINFO_ROOT/usr/share/deviceinfo/deviceinfo" ] && . ...
#
# Переменную SOURCE_DEVICEINFO_ROOT не задаёт ни pmbootstrap, ни что-либо ещё
# в chroot - grep по всему дереву находит только сам этот файл. Значит она
# пуста, и source_deviceinfo подхватывает /usr/share/deviceinfo/deviceinfo.
#
# Внутри chroot этот файл принадлежит пакету device-xiaomi-moonstone, и
# pmbootstrap ставит его из репозитория edge, а не из рабочей копии pmaports.
# Поэтому правка cache_git/pmaports/.../deviceinfo на него не влияет нисколько:
# можно менять сколько угодно, boot-deploy этого не увидит.
#
# Отсюда и две ошибки, которые выглядели не связанными:
#
#   error: unsupported page size 0
#       boot-deploy-functions.sh зовёт mkbootimg --pagesize
#       "${deviceinfo_flash_pagesize}". Переменной deviceinfo_flash_pagesize в
#       файле нет, значит "" превращается в 0. Рядом объявлена
#       deviceinfo_bootimg_pagesize, но её читает pmbootstrap, а не mkbootimg, -
#       поэтому ошибка выглядит загадочно: pagesize в файле есть и он верный.
#
#   ERROR: Unable to find sm6375-xiaomi-moonstone-2.dtb
#       К этому dtb претензий нет: он нужен mainline, а панель обслуживает
#       только SDE. Но ошибка была не в отсутствии dtb как таковом, а в том,
#       что на самой первой попытке пакет ядра ещё не был распакован. На
#       следующих попытках dtb появился, и сборка прошла дальше. То есть первая
#       ошибка была не настоящей проблемой, а следствием порядка установки.

set -eu

# Путь ищем сами, потому что под sudo $HOME=/root, и $HOME/pmos_kernel
# не существует. Порядок: аргумент, потом $HOME, потом домашний каталог
# вызывающего пользователя - последнее нужно, потому что скрипт обычно
# запускают через sudo, и $HOME там указывает на /root.
# Домашний каталог берём из passwd, а не захардкодиваем: в репозитории
# не должно быть чужих путей.
SUDO_HOME=$(getent passwd "${SUDO_USER:-$(id -un)}" 2>/dev/null | cut -d: -f6 || true)

for candidate in \
	"${1:-}" \
	"$HOME/pmos_kernel/chroot_rootfs_xiaomi-moonstone" \
	"${SUDO_HOME:-}/pmos_kernel/chroot_rootfs_xiaomi-moonstone" \
	"${SUDO_HOME:-}/pmos-kernel/chroot_rootfs_xiaomi-moonstone"
do
	[ -n "$candidate" ] || continue
	if [ -f "$candidate/usr/share/deviceinfo/deviceinfo" ]; then
		CHROOT="$candidate"
		break
	fi
done

if [ -z "${CHROOT:-}" ]; then
	echo "ОШИБКА: не нашёл usr/share/deviceinfo/deviceinfo ни в одном из:" >&2
	echo "  ${1:-<аргумент>}" >&2
	echo "  $HOME/pmos_kernel/chroot_rootfs_xiaomi-moonstone" >&2
	echo "  ${SUDO_HOME:-<домашний каталог>}/pmos_kernel/chroot_rootfs_xiaomi-moonstone" >&2
	echo "Сначала прогони pmbootstrap install хотя бы до шага mkinitfs." >&2
	exit 1
fi

F="$CHROOT/usr/share/deviceinfo/deviceinfo"
echo "Правлю: $F"

if [ ! -w "$F" ]; then
	echo "ОШИБКА: $F не доступен для записи. Запусти скрипт от root." >&2
	exit 1
fi

[ -e "$F.orig" ] || cp -p "$F" "$F.orig"

python3 - "$F" <<'PY'
import re
import sys

path = sys.argv[1]
with open(path, encoding='utf-8') as f:
    src = f.read()
orig = src

PAGESIZE = '''

# deviceinfo_flash_pagesize - не то же самое, что deviceinfo_bootimg_pagesize
# рядом. boot-deploy-functions.sh вызывает
#	mkbootimg --pagesize "${deviceinfo_flash_pagesize}"
# то есть именно эту переменную. Объявленная рядом
# deviceinfo_bootimg_pagesize использует pmbootstrap и на этот вызов не
# влияет, поэтому 0 проходит незаметно, и сборка падает с
#	error: unsupported page size 0
# 4096 - значение из стокового boot.img этого устройства.
deviceinfo_flash_pagesize="4096"'''

if 'deviceinfo_flash_pagesize' not in src:
    anchor = 'deviceinfo_bootimg_pagesize="4096"'
    if anchor not in src:
        sys.exit('ОШИБКА: нет строки deviceinfo_bootimg_pagesize, правку негде вставить')
    src = src.replace(anchor, anchor + PAGESIZE, 1)
    print('добавлено deviceinfo_flash_pagesize="4096"')
else:
    print('deviceinfo_flash_pagesize уже есть')

DTB_OFF = '''# deviceinfo_dtb убран: панель обслуживает только SDE, вендорный драйвер из
# Darkmoon-Reborn. Mainline-драйвера для неё не существует вовсе, поэтому
# отдельный dtb в boot.img не нужен, а лишний dtb во втором слоте - как раз
# тот случай, когда ядро загрузится с неверным деревом. DTB вендора уже
# вложен в сам Image.
# deviceinfo_dtb="sm6375-xiaomi-moonstone-2"'''

if re.search(r'^deviceinfo_dtb=', src, flags=re.M):
    src = re.sub(r'^deviceinfo_dtb=.*$', DTB_OFF, src, count=1, flags=re.M)
    print('убран deviceinfo_dtb')
else:
    print('deviceinfo_dtb уже отсутствует')

if src != orig:
    with open(path, 'w', encoding='utf-8') as f:
        f.write(src)
    print('файл обновлён')
else:
    print('изменений не потребовалось')
PY

echo
echo "=== итоговый deviceinfo (без комментариев) ==="
grep -vE '^\s*#|^$' "$F"

echo
echo "=== проверка: значение видится ==="
# shellcheck disable=SC1090
. "$F"
echo "deviceinfo_flash_pagesize=[$deviceinfo_flash_pagesize]"
echo "deviceinfo_bootimg_pagesize=[$deviceinfo_bootimg_pagesize]"
echo "deviceinfo_dtb=[$deviceinfo_dtb]"
echo "deviceinfo_arch=[$deviceinfo_arch]"