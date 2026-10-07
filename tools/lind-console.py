#!/usr/bin/env python3
# lind-console.py - read the phone's debug console over USB CDC ACM.
#
# The boot images built by tools/pmos-debug-initramfs.sh write their diagnostics
# into /dev/ttyGS0 on the phone, line by line, and the phone shows up here as
# /dev/ttyACM0. There is no kernel console on this device at all -
# CONFIG_FRAMEBUFFER_CONSOLE, CONFIG_VT and CONFIG_DRM_FBDEV_EMULATION are all
# unset - so this port is the only way to see anything.
#
# Two things this script has to get right, both learned the hard way:
#
#   * The device re-enumerates. When the gadget is rebuilt, or the phone
#     reboots, /dev/ttyACM0 disappears and comes back. A reader that opens it
#     once and never checks sees nothing at all, which looks exactly like the
#     phone having no output.
#   * Baud rate and line discipline. The port defaults to whatever the ACM
#     descriptor says, so set raw 115200 explicitly.
#
# Usage:
#   lind-console.py [seconds]          default 180
#   lind-console.py --send off         send a command, then read the reply
#
# Commands understood by the init built in pmos-pmos-initramfs.sh:
#   off     poweroff -f        there is no fastboot poweroff, the protocol only
#                             defines reboot, so this is the only way to power
#                             the phone off over USB
#   reboot  poweroff -f        the kernel's reboot is a poweroff here too
#   report  re-print the whole report
#   mount   try to mount pmOS_root again
#
# SPDX-License-Identifier: GPL-2.0-only

import glob
import os
import select
import signal
import sys
import termios
import time

BAUD = 115200


def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    # tcsetattr на только что появившемся USB CDC ACM отдаёт EINVAL: устройство
    # ещё не готово принимать настройки. Это не мешает читать, поэтому ошибку
    # глотаем - иначе скрипт падал бы на ровно том порте, ради которого он и
    # написан. Скорость по умолчанию для этого дескриптора и так 115200.
    try:
        attr = termios.tcgetattr(fd)
        attr[0] = 0            # iflag: raw
        attr[1] = 0            # oflag: raw
        attr[3] = 0            # lflag: no echo, no canonical, no signals
        attr[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        attr[4] = BAUD
        attr[5] = BAUD
        termios.tcsetattr(fd, termios.TCSANOW, attr)
    except (termios.error, OSError):
        pass
    return fd


def main():
    args = sys.argv[1:]
    if args and args[0] in ('-h', '--help'):
        print(__doc__ or 'lind-console.py [seconds] | --send <команда> [секунды]')
        print('команды: off, reboot, report, mount')
        return
    send = None
    if args and args[0] == '--send':
        if len(args) < 2:
            sys.exit('нужна команда: lind-console.py --send off')
        send = args[1]
        args = args[2:]
    seconds = float(args[0]) if args else 180.0
    deadline = time.time() + seconds
    fd = None
    buf = bytearray()
    opened_at = None

    def bail(*_):
        if fd is not None:
            try:
                os.close(fd)
            except OSError:
                pass
        if buf:
            sys.stdout.write(buf.decode('utf-8', 'replace'))
        sys.stdout.flush()
        sys.exit(0)

    signal.signal(signal.SIGINT, bail)
    signal.signal(signal.SIGTERM, bail)

    sys.stderr.write(f"жду порт, до {int(seconds)} с\n")
    while time.time() < deadline:
        ports = sorted(glob.glob('/dev/ttyACM*'))
        if not ports:
            if fd is not None:
                try:
                    os.close(fd)
                except OSError:
                    pass
                fd = None
            time.sleep(0.3)
            continue
        path = ports[0]
        if fd is None:
            try:
                fd = open_port(path)
            except OSError as exc:
                sys.stderr.write(f"{path}: {exc}\n")
                fd = None
                time.sleep(0.3)
                continue
            opened_at = time.time()
            sys.stderr.write(f"порт {path}\n")
            if send is not None:
                try:
                    os.write(fd, (send + '\n').encode())
                    sys.stderr.write(f"отправлено: {send}\n")
                except OSError as exc:
                    sys.stderr.write(f"не отправить: {exc}\n")
            sys.stderr.flush()
        ready, _, _ = select.select([fd], [], [], 0.3)
        if ready:
            try:
                data = os.read(fd, 65536)
            except BlockingIOError:
                continue
            except OSError:
                fd = None
                continue
            if data:
                buf += data
                opened_at = time.time()
                # Echo as it arrives, so a long boot is readable live.
                sys.stdout.write(data.decode('utf-8', 'replace'))
                sys.stdout.flush()
        elif buf and opened_at and time.time() - opened_at > 25:
            break
        if len(buf) > 400000:
            break

    if fd is not None:
        try:
            os.close(fd)
        except OSError:
            pass
    sys.stderr.write(f"\nитого {len(buf)} байт\n")


if __name__ == '__main__':
    main()