#!/usr/bin/env bash
#
# build-zip.sh - полная сборка: ядро -> boot.img -> pmOS zip -> патчи -> проверка.
#
# Это единая точка входа. Раньше путь был размазан по пяти скриптам, и это
# стоило реального времени: собрав ядро не из того дерева, я получил образ
# с localversion "-Protium" вместо "-Darkmoon-Reborn" и не сразу понял,
# в чём дело. Здесь порядок зафиксирован и каждый шаг проверяет себя.
#
# Что где лежит
# --------------
#   ~/dm-kernel/                  дерево ядра, build.sh кладёт Image в ~/ak3-darkmoon
#   ~/lindroid-kernel/            этот проект: tools/, docs/, out/
#   ~/pmos_kernel/                рабочий каталог pmbootstrap с chroot'ами
#   ~/ak3-darkmoon/Image          эталон "наше ядро", его читает pmos-pmos-initramfs.sh
#   ~/kernel_xiaomi_stone/        ЧУЖОЕ ядро "Protium". Не путать с dm-kernel.
#                                 Там же stock/boot.img для отката на Android.
#
# Использование
# -------------
#   ./build-zip.sh                 пересобрать boot.img и zip из готового Image
#   ./build-zip.sh --kernel        плюс пересобрать само ядро (~20 минут)
#   ./build-zip.sh --boot IMG      взять указанный boot.img вместо собранного
#   ./build-zip.sh --ui-console    pmOS с консолью (по умолчанию)
#   ./build-zip.sh --ui-weston     pmOS с weston
#   ./build-zip.sh --skip-pmos     только boot.img, pmOS zip не пересобирать
#
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS="$HERE/tools"
OUT="$HERE/out"

KERNEL_TREE="$HOME/dm-kernel"
AK3="$HOME/ak3-darkmoon"
PMS_WORK="$HOME/pmos_kernel"
STOCK="$HOME/kernel_xiaomi_stone/stock/boot.img"
DEVICE="xiaomi-moonstone"

DO_KERNEL=0
DO_PMOS=1
BOOT_IMG=""
UI="console"

say()  { printf '\n\033[1m=== %s\033[0m\n' "$*"; }
info() { printf '  %s\n' "$*"; }
die()  { printf '\033[31mошибка: %s\033[0m\n' "$*" >&2; exit 1; }

# Разбор аргументов на while, а не на for: `--boot IMG` нужно забрать
# следующий аргумент, а shift внутри `for ... in "$@"` не двигает список,
# по которому идёт цикл, и IMG уезжал в "непонятный аргумент".
while [ $# -gt 0 ]; do
	case "$1" in
		--kernel)      DO_KERNEL=1; shift ;;
		--skip-pmos)   DO_PMOS=0; shift ;;
		--ui-console)  UI="console"; shift ;;
		--ui-weston)   UI="weston"; shift ;;
		--ui-surfbgtk) UI="surfbgtk"; shift ;;
		--boot)        BOOT_IMG="${2:?--boot требует путь}"; shift 2 ;;
		-h|--help)     sed -n '/^# Использование/,/^$/p' "$0" | sed 's/^# \?//'; exit 0 ;;
		*)             die "непонятный аргумент: $1" ;;
	esac
done

mkdir -p "$OUT"

# --------------------------------------------------------------- ядро
if [ "$DO_KERNEL" = 1 ]; then
	say "собираю ядро"
	[ -d "$KERNEL_TREE" ] || die "нет $KERNEL_TREE"
	# Только в dm-kernel. В kernel_xiaomi_stone другое ядро, Protium.
	grep -q 'LOCALVERSION="-Darkmoon-Reborn"' \
		"$KERNEL_TREE/arch/arm64/configs/stone_defconfig" ||
		die "в $KERNEL_TREE не наше ядро. Нужен dm-kernel, не kernel_xiaomi_stone."
	( cd "$KERNEL_TREE" && bash build.sh )
fi

[ -f "$AK3/Image" ] || die "нет $AK3/Image - ядро ни разу не собрано"
info "Image: $(stat -c%s "$AK3/Image") байт, $(date -r "$AK3/Image" '+%d.%m %H:%M')"

# Проверяем, что это действительно наше ядро. Эта проверка окупила бы любую
# свою стоимость: version string внутри образа - единственный надёжный
# признак, и однажды мы собрали не то дерево и потеряли на этом час.
VER=$(strings -n 40 "$AK3/Image" | grep -m1 -o 'Linux version [^ ]*' || true)
case "$VER" in
	*Darkmoon-Reborn*) info "версия: $VER  верно" ;;
	*)                 die "в Image строка '$VER', ожидался Darkmoon-Reborn" ;;
esac

# --------------------------------------------------------------- boot.img
if [ -z "$BOOT_IMG" ]; then
	say "собираю boot.img"
	BOOT_OUT="$OUT/boot-$UI.img"
	# cmdline оставляем пустым: стоковый тоже пустой, а pmOS-ный init
	# сам ставит свой заголовок. LIND_CMDLINE сюда не нужен.
	bash "$TOOLS/pmos-pmos-initramfs.sh" "$BOOT_OUT"
	BOOT_IMG="$BOOT_OUT"
fi
[ -f "$BOOT_IMG" ] || die "нет $BOOT_IMG"
BOOT_MD5=$(md5sum "$BOOT_IMG" | cut -d' ' -f1)
info "boot.img: $BOOT_IMG"
info "         $BOOT_MD5  $(stat -c%s "$BOOT_IMG") байт"

[ "$DO_PMOS" = 1 ] || { say "готово: $BOOT_IMG"; exit 0; }

# --------------------------------------------------------------- pmOS zip
say "собираю pmOS recovery zip (ui=$UI)"
command -v pmbootstrap >/dev/null 2>&1 || die "нет pmbootstrap в PATH"

# pmbootstrap дёргает sudo на каждый chroot-шаг ("sudo mkdir -p .../chroot_native/dev"
# и так далее). Без TTY запрос пароля некуда показать, и установка обрывается.
# Но sudo по умолчанию привязывает билет к терминалу (tty_tickets), поэтому
# "sudo -v" из твоего терминала сюда не долетает: у этого процесса другой
# сеанс и нет TTY. Лечится только запуском скрипта у тебя в терминале.
if [ "$DO_PMOS" = 1 ] && ! sudo -n true 2>/dev/null; then
	if [ -t 0 ]; then
		die "нужен sudo. Выполни:  sudo -v
     и запусти скрипт снова в этом же терминале."
	else
		die "нужен sudo, а у этого процесса нет TTY.
     sudo привязывает билет к терминалу, поэтому твой 'sudo -v' сюда не
     долетает. Запусти сборку у себя в терминале:

         cd ~/lindroid-kernel && ./build-zip.sh --ui-console

     Если и там запросит - сначала 'sudo -v'."
	fi
fi

# pmbootstrap install не принимает --device, --ui и --add-repository: это
# параметры init, и в этой версии конфиг лежит не в файле, а выводится из
# самого рабочего каталога. Проверяем, что там то, что мы просим.
STATUS=$(cd "$PMS_WORK" && pmbootstrap status 2>&1 | tr -d '\033' | sed 's/\[[0-9;]*m//g')
HAVE_UI=$(printf '%s\n' "$STATUS" | sed -n 's/.*UI: *\([a-z]*\).*/\1/p')
HAVE_DEV=$(printf '%s\n' "$STATUS" | sed -n 's/.*Device: *\([a-z0-9-]*\).*/\1/p')
info "в рабочем каталоге: device=$HAVE_DEV ui=$HAVE_UI"

[ "$HAVE_DEV" = "$DEVICE" ] ||
	die "в $PMS_WORK настроено устройство '$HAVE_DEV', а нужно '$DEVICE'"

if [ "$HAVE_UI" != "$UI" ]; then
	die "в $PMS_WORK уже выбрано окружение '$HAVE_UI', а запрошено '$UI'.
     Смена окружения - это pmbootstrap init заново, он трогает chroot'ы и
     тянет пакеты. Скажи, и я прогоню отдельным шагом."
fi

# --password снимает интерактивный вопрос "Choose a password for the user
# 'drd4'" - без TTY он обрывает установку. Это фиктивный пароль для
# автоматизации, и в готовой системе его всё равно нет: pmos-patch-rootfs.py
# с пустым полем убирает блокировку root в etc/shadow.
( cd "$PMS_WORK" && pmbootstrap install --android-recovery-zip \
	--password lindroid ) 2>&1 | tail -5 ||
	die "pmbootstrap install не отработал"

# Где лежит zip. pmbootstrap 3.11 с --android-recovery-zip кладёт его не в
# images/, а внутрь buildroot chroot'а, в var/lib/...-recovery-installer/.
# Старый путь оставлен как запасной: на других версиях он работал.
PMOS_ZIP=$(ls -t "$PMS_WORK"/chroot_buildroot_aarch64/var/lib/postmarketos-android-recovery-installer/*.zip 2>/dev/null | head -1 || true)
[ -n "$PMOS_ZIP" ] || PMOS_ZIP=$(ls -t "$PMS_WORK"/images/*pmos*.zip 2>/dev/null | head -1 || true)
[ -n "$PMOS_ZIP" ] || die "pmbootstrap отработал, но zip не найден. Искали в:
     $PMS_WORK/chroot_buildroot_aarch64/var/lib/postmarketos-android-recovery-installer/
     $PMS_WORK/images/"
info "исходный zip: $PMOS_ZIP ($(stat -c%s "$PMOS_ZIP") байт)"

# --------------------------------------------------------------- патчи
say "правлю A/B: INSTALL_PARTITION -> userdata"
# Установщик переразмечает свой целевой раздел, а boot_a всего 128 МиБ -
# меньше, чем нужно для 1.5-гигабайтной системы. userdata не слот и свободен.
AB_ZIP="$OUT/pmos-$UI-ab.zip"
# pmos-patch-ab.sh правит zip на месте и принимает только путь и слот,
# поэтому работаем с копией: zip от pmbootstrap остаётся нетронутым, и
# повторный запуск не накапливает правки.
cp -p "$PMOS_ZIP" "$AB_ZIP"
bash "$TOOLS/pmos-patch-ab.sh" "$AB_ZIP" _a

say "правлю rootfs: boot.img, getty на USB, root без пароля"
# Три правки внутри rootfs.tar.gz. Без них свежая установка загрузится,
# но останется недоступной: getty на ttyGS0 нет, а root заблокирован.
ROOT_ZIP="$OUT/pmos-$UI-final.zip"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
python3 "$TOOLS/pmos-patch-rootfs.py" "$AB_ZIP" "$ROOT_ZIP" \
	--boot "$BOOT_IMG" --unlock-root --usb-getty ttyGS0 \
	--work "$WORK" || die "pmos-patch-rootfs.py не отработал"

# --------------------------------------------------------------- проверка
say "проверяю итог"
unzip -l "$ROOT_ZIP" | grep -E 'update-binary|rootfs\.tar\.gz' |
	sed 's/^/  /' || die "в zip нет update-binary или rootfs.tar.gz"

unzip -p "$ROOT_ZIP" rootfs.tar.gz | tar -tzvf - 2>/dev/null |
	grep -E ' \./boot/boot\.img$' | sed 's/^/  /' ||
	die "в rootfs.tar.gz нет ./boot/boot.img"

# Сверяем, что внутри tar лежит именно наш образ, а не стоковый.
INNER=$(unzip -p "$ROOT_ZIP" rootfs.tar.gz |
	tar -xOzf - ./boot/boot.img 2>/dev/null | md5sum | cut -d' ' -f1)
[ "$INNER" = "$BOOT_MD5" ] ||
	die "внутри rootfs.tar.gz лежит $INNER, а мы собирали $BOOT_MD5"
info "boot.img внутри архива совпадает"

unzip -p "$ROOT_ZIP" rootfs.tar.gz | tar -xOzf - ./etc/inittab 2>/dev/null |
	grep ttyGS0 | sed 's/^/  inittab: /' || die "в inittab нет getty на ttyGS0"
unzip -p "$ROOT_ZIP" rootfs.tar.gz | tar -xOzf - ./etc/shadow 2>/dev/null |
	grep '^root:' | sed 's/^/  shadow:  /'

FINAL_MD5=$(md5sum "$ROOT_ZIP" | cut -d' ' -f1)
say "готово"
info "$ROOT_ZIP"
info "$(stat -c%s "$ROOT_ZIP") байт"
info "md5 $FINAL_MD5"
info "boot.img $BOOT_MD5"
