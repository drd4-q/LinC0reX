#!/usr/bin/env python3
"""
Walk a DSI command blob the way dsi_panel_get_cmd_pkt_count() does, so the
te-patch cannot be added on a guess.

The kernel parser is:

    while (length >= 7) {
            packet_length = 7 + ((data[5] << 8) | data[6]);
            if (packet_length > length) return -EINVAL;
            length -= packet_length;
            data   += packet_length;
            count++;
    }

Each packet is therefore 7 header bytes followed by tx_len payload bytes, and
the walk stops only when fewer than 7 bytes remain. Padding left over at the
end is NOT harmless: it gets parsed as another packet, or trips the format
check.

DT cells are 32 bits each, so a <...> blob is a byte stream whose length is a
multiple of 4 - which means the author has to make the packets add up exactly.
That is the thing worth verifying before editing.

SPDX-License-Identifier: GPL-2.0-only
"""

import re
import sys

TYPES = {
    0x06: "SET_ADDRESS_MODE",
    0x15: "DCS_SHORT_WRITE",
    0x29: "SHORT_WRITE",
    0x23: "SHORT_WRITE_NOPARM",
    0x39: "DCS_LONG_WRITE",
    0x6E: "LONG_WRITE",
    0x29: "SHORT_WRITE",
}


def cells(text):
    """Every <0x..> cell in order, as 32-bit values."""
    return [int(m, 16) for m in re.findall(r"0x([0-9a-fA-F]+)", text)]


def to_bytes(vals):
    """DT cells are stored big-endian."""
    return b"".join(v.to_bytes(4, "big") for v in vals)


def walk(blob, label):
    print(f"=== {label}")
    print(f"    длина: {len(blob)} байт ({len(blob) % 4} по модулю 4)")
    off, n = 0, 0
    while len(blob) - off >= 7:
        t, last = blob[off], blob[off + 1]
        ch, flags, wait = blob[off + 2], blob[off + 3], blob[off + 4]
        tx_len = (blob[off + 5] << 8) | blob[off + 6]
        if 7 + tx_len > len(blob) - off:
            print(f"    пакет {n}: НЕВЕРНЫЙ len={tx_len}, осталось "
                  f"{len(blob) - off}")
            break
        payload = blob[off + 7: off + 7 + tx_len]
        name = TYPES.get(t, f"0x{t:02x}")
        reg = f"0x{payload[0]:02x}" if payload else "-"
        print(f"    пакет {n}: type=0x{t:02x} {name:20s} last={last} "
              f"ch={ch} flags=0x{flags:02x} wait={wait} "
              f"len={tx_len:3d} reg={reg} {payload.hex(' ')}")
        off += 7 + tx_len
        n += 1
    tail = len(blob) - off
    if tail:
        print(f"    хвост {tail} байт: {blob[off:].hex(' ')}")
        print("    -> хвост разберётся как ещё один пакет!" if tail >= 7
              else "    -> хвост меньше 7 байт, парсер остановится")
    else:
        print("    хвоста нет, парсер остановится ровно")
    print()


if __name__ == "__main__":
    src = open(sys.argv[1]).read()

    m = re.search(r"qcom,mdss-dsi-on-command\s*=\s*<([^>]*)>", src)
    if m:
        walk(to_bytes(cells(m.group(1))), "существующий on-command")

    m = re.search(r"qcom,mdss-dsi-pre-off-command\s*=\s*\[([^\]]*)\]",
                  src)
    if m:
        raw = bytes(int(x, 16) for x in m.group(1).split())
        walk(raw, "существующий pre-off-command (байтовый массив)")

    m = re.search(r"qcom,mdss-dsi-post-panel-on-command\s*=\s*<([^>]*)>",
                  src)
    if m:
        walk(to_bytes(cells(m.group(1))), "мой post-panel-on-command")
