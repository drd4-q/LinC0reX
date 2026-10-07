#!/bin/sh
# pmos-fixup-sysroot.sh - запуск из отладочного шелла initramfs.
#
# Задача: попасть внутрь системы, не зная пароля root, и оставить после
# себя постоянный доступ через USB. Сам initramfs живёт в tmpfs и умирает
# при перезагрузке, поэтому всё ценное правим в /sysroot - это настоящая
# корневая ФС, изменения в ней переживут перезагрузку.
#
# Отладочный шелл не требует пароля: это /bin/sh от root в tmpfs.

say() { echo "--- $*"; }

SRC=/dev/sda19
LOOP=/dev/loop0

say "отвязываю $LOOP от всего, что на нём висит"
losetup -d "$LOOP" 2>/dev/null

# --direct-io=on из pmOS убираем намеренно: userdata имеет логический сектор
# 4096 байт, а loop создаётся с блоком 512. Прямой ввод-вывод уходит в
# очередь подложки без разбиения, и чтение ext4 падает с
# "blk_update_request: I/O error" -> "unable to read superblock".
say "цепляю $SRC без direct-io"
losetup -P "$LOOP" "$SRC" || { say "НЕ СМОГ прицепить loop"; exit 1; }
mdev -s >/dev/null 2>&1
sleep 1

say "проверяю метки"
blkid "${LOOP}p1" "${LOOP}p2" || say "blkid ничего не показал"

if [ ! -b "${LOOP}p2" ]; then
	say "раздела ${LOOP}p2 нет - дальше идти некуда"
	losetup -a
	exit 1
fi

say "монтирую корень в /sysroot"
mkdir -p /sysroot
if ! mount "${LOOP}p2" /sysroot 2>/dev/null; then
	say "mount не прошёл, пробую rw"
	mount -o rw "${LOOP}p2" /sysroot || { say "НЕ СМОГ смонтировать"; exit 1; }
fi

say "содержимое корня"
ls /sysroot | head -12

# Постоянный доступ по USB. В штатной системе гетти на ttyGS0 не поднимается,
# поэтому /dev/ttyACM0 на стороне PC есть, но на нём никто не слушает.
say "прописываю гетти на ttyGS0 в /sysroot/etc/inittab"
if [ -f /sysroot/etc/inittab ]; then
	grep -q 'ttyGS0' /sysroot/etc/inittab || \
		echo 'ttyGS0::respawn:/sbin/getty -L 115200 ttyGS0 vt100' >> /sysroot/etc/inittab
	tail -3 /sysroot/etc/inittab
else
	say "нет /sysroot/etc/inittab - это не pmOS корень, осторожно"
fi

# Пароль нам неизвестен, а по экрану его не ввести. Ставим пустой.
say "сбрасываю пароль root на пустой"
if [ -f /sysroot/etc/shadow ]; then
	sed -i 's/^root:[^:]*:/root::/' /sysroot/etc/shadow
	grep '^root:' /sysroot/etc/shadow
else
	say "нет /sysroot/etc/shadow"
fi

say "готово. Перезагрузи телефон - доступ будет через /dev/ttyACM0"
say "вход: root, пароль пустой"
