#!/usr/bin/env bash
#
# pmos-inject-bootimg.sh - put a boot.img into a pmOS recovery flashable zip.
#
# Why this exists
# ---------------
# pmos-lind-kernel.sh rebuilds a boot.img correctly, but the boot.img is not
# where either of us assumed. It is not in the zip:
#
#   $ unzip -l pmos-xiaomi-moonstone.zip
#     META-INF/...
#     chroot/...
#     rootfs.tar.gz          <- the boot.img is in here
#
# The installer decides what to flash from install_options, and with
# ISOREC=false it takes this branch of pmos_install:
#
#     if [ "$ISOREC" = "true" ]; then
#             dd if=/mnt/pmOS/boot/vmlinuz   of="$KERNEL_PARTITION"
#             gunzip -c /mnt/pmOS/boot/initramfs | lzop > "$INITFS_PARTITION"
#     else
#             dd if=/mnt/pmOS/boot/boot.img  of="$BOOT_PARTITION"
#     fi
#
# and /mnt/pmOS/boot is populated by extracting rootfs.tar.gz over the rootfs
# partition. So the file to replace is the tar member ./boot/boot.img inside
# rootfs.tar.gz inside the zip. Replacing a "boot.img" in the zip root would
# quietly do nothing.
#
# Why one pass
# ------------
# rootfs.tar.gz is 256 MB compressed and 1.5 GB raw. Unpacking it to disk,
# editing, and re-packing works but needs the space and a second copy. Here the
# tar is streamed: every member is copied across with its original TarInfo, so
# ownership, modes and mtimes are preserved byte for byte, and only the one
# member is swapped.
#
# Ownership is not a detail here. pmbootstrap builds this tar as root, so every
# member is uid 0. Rebuilding it as a normal user without --owner=root would
# hand recovery a filesystem whose files belong to uid 1000, and that is the
# kind of thing that fails much later and far from the cause.
#
# Usage:
#   pmos-inject-bootimg.sh <pmos.zip> <boot.img> [out.zip]
#
# SPDX-License-Identifier: GPL-2.0-only

set -e

PMOSZIP="${1:?нужен путь к pmos recovery zip}"
NEWBOOT="${2:?нужен путь к новому boot.img}"
OUTZIP="${3:-${PMOSZIP%.zip}-lind.zip}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

say() { printf '%s\n' "$*"; }
die() { printf 'ошибка: %s\n' "$*" >&2; exit 1; }

command -v zip      >/dev/null 2>&1 || die "нет zip"
command -v unzip    >/dev/null 2>&1 || die "нет unzip"
command -v python3  >/dev/null 2>&1 || die "нет python3"

[ -f "$PMOSZIP" ] || die "нет $PMOSZIP"
[ -f "$NEWBOOT" ] || die "нет $NEWBOOT"

say "=== исходный zip"
unzip -l "$PMOSZIP" | awk 'NR>3 && NF>=4 {print "  " $1 "  " $4}' | head -8

unzip -l "$PMOSZIP" | grep -q ' rootfs\.tar\.gz$' ||
	die "в zip нет rootfs.tar.gz - это не тот zip"

say "=== новый boot.img: $(stat -c%s "$NEWBOOT") байт"

# Build the new rootfs.tar.gz. Two steps, tar then gzip, the same way
# pmbootstrap does it (`tar -pcf rootfs.tar ...` then `gzip -f1`).
#
# Doing it in one pass by handing tarfile a GzipFile looks tidier and is wrong.
# tarfile's streaming writer closes the file object it was given when it closes,
# and the GzipFile's own close then runs against an already-closed handle. The
# gzip trailer is never written, so the archive is silently truncated: it still
# unpacks, and it is still missing whatever came after the cut. Measured here:
# 7986 members written instead of 8026, and `gzip -t` reporting an unexpected
# end of file. A member count check is the only thing that catches it.
python3 - "$PMOSZIP" "$NEWBOOT" "$WORK/rootfs.tar" <<'PY'
import gzip
import io
import sys
import tarfile
import zipfile

pmoszip, newboot, outtar = sys.argv[1:4]
TARGET = './boot/boot.img'

with open(newboot, 'rb') as f:
    newdata = f.read()
newsize = len(newdata)

swapped = 0
seen = 0

with zipfile.ZipFile(pmoszip) as z:
    with z.open('rootfs.tar.gz') as raw_in:
        # The member is stored, not deflated: zip holds the gzip bytes as they
        # are, so tarfile needs the decompressing wrapper or it reads a gzip
        # header as a tar header and stops with "invalid header".
        gz_in = gzip.GzipFile(fileobj=raw_in, mode='rb')
        with tarfile.open(fileobj=gz_in, mode='r|') as tin:
            with open(outtar, 'wb') as raw_out:
                with tarfile.open(fileobj=raw_out, mode='w|',
                                  format=tarfile.GNU_FORMAT) as tout:
                    for m in tin:
                        seen += 1
                        if m.name == TARGET:
                            # Keep the original TarInfo: mode, uid, gid,
                            # uname, gname, mtime. Only size changes, and the
                            # member has to be written with the new one.
                            m.size = newsize
                            tout.addfile(m, io.BytesIO(newdata))
                            swapped += 1
                            print(f'  заменён {m.name}: '
                                  f'{m.uid}:{m.gid} {m.mode:o} {newsize} байт')
                            continue
                        f = tin.extractfile(m) if m.isreg() else None
                        tout.addfile(m, f)

if swapped != 1:
    sys.exit(f'ОШИБКА: член {TARGET} встретился {swapped} раз, ожидался 1')
print(f'  всего членов: {seen}')
PY

ORIG_MEMBERS=$(unzip -p "$PMOSZIP" rootfs.tar.gz | tar -tz 2>/dev/null | wc -l)
NEW_MEMBERS=$(tar -tf "$WORK/rootfs.tar" 2>/dev/null | wc -l)
say "=== членов: было $ORIG_MEMBERS, стало $NEW_MEMBERS"
[ "$ORIG_MEMBERS" = "$NEW_MEMBERS" ] ||
	die "число членов изменилось - архив неполон"

# compresslevel=1 matches `gzip -f1` in the pmbootstrap log, so the result is
# not needlessly larger than the original.
gzip -1 -c "$WORK/rootfs.tar" > "$WORK/rootfs.tar.gz" ||
	die "gzip не сжал rootfs.tar"

gzip -t "$WORK/rootfs.tar.gz" 2>/dev/null || die "gzip -t не прошёл"
tar -tzf "$WORK/rootfs.tar.gz" >/dev/null 2>&1 ||
	die "tar не читает собранный rootfs.tar.gz"

say "=== собран rootfs.tar.gz: $(stat -c%s "$WORK/rootfs.tar.gz") байт"

# Rebuild the zip: copy the original, then replace the single member. Copying
# first keeps META-INF/com/google/android/update-binary byte-identical, which
# is what the recovery actually reads to decide what to run.
say "=== пересобираю zip"
cp -p "$PMOSZIP" "$OUTZIP"
( cd "$WORK" && zip -q "$OUTZIP" rootfs.tar.gz ) ||
	die "zip не обновил rootfs.tar.gz"

say ""
say "=== проверка"
N=$(unzip -l "$OUTZIP" | grep -c ' rootfs\.tar\.gz$' || true)
[ "$N" = "1" ] || die "в итоговом zip rootfs.tar.gz встречается $N раз"

unzip -p "$OUTZIP" rootfs.tar.gz | tar -tzvf - 2>/dev/null |
	grep -E ' \./boot/boot\.img$' |
	sed 's/^/  /' || die "в итоговом tar нет ./boot/boot.img"

unzip -l "$OUTZIP" | grep -E 'update-binary|rootfs\.tar\.gz' | sed 's/^/  /'

say ""
say "готово: $OUTZIP ($(stat -c%s "$OUTZIP") байт)"