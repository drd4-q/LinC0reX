#!/usr/bin/env python3
# pmos-send.py - send a command to the phone's pmOS debug shell and read the
# reply. The counterpart of lind-console.py.
#
# The phone runs our kernel with the pmOS initramfs, and when it cannot finish
# mounting it drops to a root shell on /dev/ttyGS0, which appears on the PC as
# /dev/ttyACM0. That shell is the only way in: this kernel has no framebuffer
# console at all and the cmdline is empty, so /dev/console goes nowhere.
#
# Usage:
#   pmos-send.py 'blkid | grep pmOS' [seconds]
#
# SPDX-License-Identifier: GPL-2.0-only

import glob
import os
import select
import sys
import termios
import time

if len(sys.argv) < 2:
    sys.exit(__doc__ or 'pmos-send.py <команда> [секунды]')
cmd = sys.argv[1]
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 45.0

# Нумерация ttyACM сдвигается при каждом переподключении USB, и на телефоне
# может быть два ACM - один от нашей вставки в pmOS, второй от его же
# конфигурации. Живой порт не обязательно первый по имени, поэтому пробуем
# все и берём тот, который отвечает.
ports = sorted(glob.glob('/dev/ttyACM*'))
if not ports:
    sys.exit('порта нет: телефон не загрузился с нашим образом, '
             'либо порт не поднялся')

fd = None
for port in ports:
    try:
        cand = os.open(port, os.O_RDWR | os.O_NOCTTY)
    except OSError:
        continue
    time.sleep(0.3)
    try:
        os.write(cand, b'\n')
    except OSError:
        os.close(cand)
        continue
    ready, _, _ = select.select([cand], [], [], 1.0)
    if ready:
        try:
            if os.read(cand, 4096):
                fd = cand
                sys.stderr.write(f'живой порт: {port}\n')
                break
        except OSError:
            pass
    os.close(cand)

if fd is None:
    sys.exit(f'ни один порт не отвечает: {ports}')
try:
    attr = termios.tcgetattr(fd)
    attr[0] = 0
    attr[1] = 0
    attr[3] = 0
    attr[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attr[4] = termios.B115200
    attr[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, attr)
except (termios.error, OSError):
    pass

time.sleep(1)
try:
    os.write(fd, (cmd + '\n').encode())
except OSError as exc:
    sys.exit(f'не отправить: {exc}')

buf = bytearray()
t0 = time.time()
while time.time() - t0 < secs:
    ready, _, _ = select.select([fd], [], [], 0.5)
    if ready:
        try:
            data = os.read(fd, 65536)
        except OSError:
            break
        if data:
            buf += data
            t0 = time.time()

os.close(fd)
sys.stdout.write(buf.decode('utf-8', 'replace'))
sys.stderr.write(f'\nпрочитано {len(buf)} байт\n')
