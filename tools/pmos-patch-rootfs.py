#!/usr/bin/env python3
"""
pmos-patch-rootfs.py - правка членов rootfs.tar.gz внутри pmOS recovery zip.

Зачем это отдельный инструмент
------------------------------
pmOS-ный recovery zip устроен так:

    META-INF/com/google/android/update-binary   <- что запускает recovery
    chroot/...                                  <- скрипты установщика (60 файлов)
    rootfs.tar.gz                               <- а вот здесь вся система

Файла /etc/inittab в zip нет, он лежит внутри rootfs.tar.gz как ./etc/inittab.
pmos-inject-bootimg.sh умеет менять там ровно один член - ./boot/boot.img.
Нам нужно три, причём два из них не заменить, а отредактировать, поэтому
инструмент обобщён.

Что и зачем правим
------------------
./boot/boot.img   заменяем на наш образ. Собранный pmbootstrap boot.img
                  загрузчик этого телефона не берёт: header_version=0,
                  page_size=4096, тогда как стоковый v3/0. Единственный
                  рабочий способ - magiskboot repack поверх стокового
                  заголовка, и его делает pmos-pmos-initramfs.sh.

./etc/inittab    дописываем getty на ttyGS0. Без него система загружается,
                  но доступа нет: штатный скрипт pmOS поднимает только NCM,
                  а не ACM, поэтому /dev/ttyGS0 не создаётся и на USB-порту
                  никто не слушает. Строка переживает перезагрузку, доступ
                  становится постоянным.

./etc/shadow     снимаем "!" с root. В исходной установке там root:!::::::,
                  то есть учётка заблокирована, а не защищена паролем. login
                  отвергает пустой пароль, и обойти это с экрана нельзя:
                  fbcon ввод не обрабатывает. С пустым полем login проходит.

Почему потоковая пересборка
---------------------------
rootfs.tar.gz - это 256 МБ в сжатом виде и 1.5 ГБ в сыром. Распаковать на
диск, отредактировать и запаковать обратно можно, но это лишнее место и вторая
копия. Здесь tar переписывается на лету: каждый член переносится с исходным
TarInfo, поэтому владелец, права и mtime сохраняются байт в байт.

Владелец здесь не мелочь. pmbootstrap собирает этот tar под root, так что
все члены принадлежат uid 0. Пересобрать его обычному пользователю без
--owner=root - значит отдать recovery файловую систему, где файлы принадлежат
uid 1000. Это ломается позже и совсем не там, где причина.

Два известных грабля, на которые уже наступали
-----------------------------------------------
1. tarfile и GzipFile в одном проходе. Если отдать GzipFile потоковому
   писателю tarfile, тот при закрытии закроет файл, который ему дали, и
   закрытие GzipFile пройдёт по уже закрытому дескриптору. Трейлер gzip не
   пишется, архив молча обрезается: он распаковывается и в нём не хватает
   всего, что после обреза. Проверка `gzip -t` это ловит, проверка числа
   членов - тоже, а вот "распаковалось же" - нет. Поэтому здесь два шага,
   ровно как у pmbootstrap: tar, потом gzip.
2. zip хранит байты gzip как есть, без разжатия. Поэтому tarfile нужен
   распаковывающая обёртка, иначе он читает заголовок gzip как заголовок tar
   и останавливается с "invalid header".

Использование
-------------
    pmos-patch-rootfs.py <zip> <out.zip> --boot <boot.img> [--unlock-root] \
                                           [--usb-getty ttyGS0]

SPDX-License-Identifier: GPL-2.0-only
"""

import argparse
import gzip
import io
import re
import shutil
import sys
import tarfile
import zipfile


def norm(name):
    """Привести имя члена к виду ./path, чтобы совпадение не зависело от
    того, как именно tar его записал."""
    while name.startswith('./'):
        name = name[2:]
    return './' + name


def patch_inittab(data, tty):
    """Дописать respawn-getty на USB-порт, если его ещё нет."""
    text = data.decode('utf-8', 'replace')
    if tty in text:
        return data, False
    line = f'{tty}::respawn:/sbin/getty 115200 {tty} vt100'
    if not text.endswith('\n'):
        text += '\n'
    text += line + '\n'
    return text.encode('utf-8'), True


def patch_shadow(data):
    """Снять блокировку root. Меняется только второе поле - пароль."""
    out = []
    changed = False
    for line in data.decode('utf-8', 'replace').split('\n'):
        if line.startswith('root:'):
            parts = line.split(':')
            if len(parts) > 1 and parts[1] not in ('',):
                parts[1] = ''
                line = ':'.join(parts)
                changed = True
        out.append(line)
    return '\n'.join(out).encode('utf-8'), changed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pmoszip')
    ap.add_argument('outzip')
    ap.add_argument('--boot', help='новый boot.img для ./boot/boot.img')
    ap.add_argument('--unlock-root', action='store_true',
                    help='снять блокировку root в ./etc/shadow')
    ap.add_argument('--usb-getty', metavar='TTY',
                    help='дописать getty на этот tty в ./etc/inittab')
    ap.add_argument('--work', required=True, help='каталог для промежуточных файлов')
    args = ap.parse_args()

    if not any((args.boot, args.unlock_root, args.usb_getty)):
        sys.exit('ОШИБКА: не задано ни одной правки')

    targets = {}
    if args.boot:
        with open(args.boot, 'rb') as f:
            targets['./boot/boot.img'] = f.read()

    edits = {}
    if args.usb_getty:
        edits['./etc/inittab'] = lambda d: patch_inittab(d, args.usb_getty)
    if args.unlock_root:
        edits['./etc/shadow'] = patch_shadow

    outtar = f'{args.work}/rootfs.tar'
    seen = 0
    done = {}

    with zipfile.ZipFile(args.pmoszip) as z:
        with z.open('rootfs.tar.gz') as raw_in:
            gz_in = gzip.GzipFile(fileobj=raw_in, mode='rb')
            with tarfile.open(fileobj=gz_in, mode='r|') as tin:
                with open(outtar, 'wb') as raw_out:
                    with tarfile.open(fileobj=raw_out, mode='w|',
                                      format=tarfile.GNU_FORMAT) as tout:
                        for m in tin:
                            seen += 1
                            key = norm(m.name)
                            if key in targets:
                                data = targets[key]
                                m.size = len(data)
                                tout.addfile(m, io.BytesIO(data))
                                done[key] = f'заменён, {len(data)} байт'
                                continue
                            if key in edits:
                                f = tin.extractfile(m)
                                data, changed = edits[key](f.read())
                                m.size = len(data)
                                tout.addfile(m, io.BytesIO(data))
                                done[key] = ('изменён' if changed
                                             else 'уже был в нужном виде')
                                continue
                            f = tin.extractfile(m) if m.isreg() else None
                            tout.addfile(m, f)

    for key, how in done.items():
        print(f'  {key}: {how}')

    for key in list(targets) + list(edits):
        if key not in done:
            sys.exit(f'ОШИБКА: член {key} в архиве не найден')

    print(f'  всего членов: {seen}')

    # -------------------------------------------------- gzip и пересборка zip
    # Два отдельных шага, и это не педантизм. Если отдать GzipFile потоковому
    # писателю tarfile, тот при закрытии закроет файл, который ему дали, и
    # закрытие GzipFile пройдёт по уже закрытому дескриптору. Трейлер gzip не
    # пишется, архив молча обрезается: он распаковывается, и в нём не
    # хватает всего, что после обреза. "Распаковалось же" эту ошибку не
    # ловит - ловят gzip -t и счётчик членов.
    #
    # compresslevel=1 повторяет "gzip -f1" в логе pmbootstrap, чтобы архив не
    # раздувался без нужды.
    outgz = f'{args.work}/rootfs.tar.gz'
    print(f'  сжимаю ({seen} членов)')
    with open(outtar, 'rb') as src, open(outgz, 'wb') as dst:
        with gzip.GzipFile(fileobj=dst, mode='wb', compresslevel=1,
                           mtime=0) as gz:
            while True:
                chunk = src.read(1 << 20)
                if not chunk:
                    break
                gz.write(chunk)

    # Проверяем то, что молча ломается.
    with gzip.open(outgz, 'rb') as gz:
        with tarfile.open(fileobj=gz, mode='r|') as t:
            n = sum(1 for _ in t)
    if n != seen:
        sys.exit(f'ОШИБКА: после сжатия членов {n}, ожидалось {seen} - '
                 f'архив обрезан')
    print(f'  сжатый архив цел, {n} членов')

    # Пересобираем zip копией оригинала: так META-INF/.../update-binary
    # остаётся байт в байт, а это то, что recovery читает, чтобы решить,
    # что вообще запускать.
    shutil.copyfile(args.pmoszip, args.outzip)
    with zipfile.ZipFile(args.pmoszip) as zin, \
         zipfile.ZipFile(args.outzip, 'w') as zout:
        for item in zin.infolist():
            if item.filename == 'rootfs.tar.gz':
                continue
            zout.writestr(item, zin.read(item.filename))
        # The member is stored, not deflated: pmbootstrap writes it that way
        # and recovery reads it back the same way.
        zout.writestr(zipfile.ZipInfo('rootfs.tar.gz'),
                      open(outgz, 'rb').read(),
                      compress_type=zipfile.ZIP_STORED)

    with zipfile.ZipFile(args.outzip) as z:
        names = z.namelist()
        if names.count('rootfs.tar.gz') != 1:
            sys.exit('ОШИБКА: rootfs.tar.gz в итоговом zip встречается '
                     f'{names.count("rootfs.tar.gz")} раз')
    print(f'  zip пересобран: {args.outzip}')


if __name__ == '__main__':
    main()
