# Runbook: installing pmOS on this phone without Android in the way

Written 2026-10-05. Everything in here was measured on the device; the reasons
are in `docs/pmos-boot-failure.md` and `docs/pmos-dtb.md`.

## The trap this avoids

The pmOS installer does work on this phone: it creates `pmOS_boot` and
`pmOS_root` inside `userdata`, and it writes our `boot.img` into `boot_a`. The
first install did exactly that - we found both ext superblocks with the right
labels by reading `userdata` off the phone.

Then the phone rebooted into Android. `userdata` had a foreign partition table,
Android could not mount it, and the root filesystem was gone. The partition
table survived; the filesystems did not. On the next boot:

```
[loop0p1] PARTUUID="0c525135-01"     <- no filesystem signature at all
[loop0p2] PARTUUID="0c525135-02"
[loop0p1] eb31                        <- expected ef53 (ext magic)
[loop0p2] 1fda
```

So the rule is: **between the install and the first pmOS boot, Android must not
run even once.** Hence the order below - recovery installs, and the phone goes
straight from recovery into pmOS.

## Why recovery is needed at all

Recovery lives inside `boot_a` on this device. Replacing `boot_a` removes it,
and `fastboot reboot recovery` then just falls back to fastboot. So recovery has
to go back in first.

The tempting alternative - have our own initramfs install pmOS - does not work:
the initramfs has no `tar` and no `mkfs.ext4`. busybox does not implement them
and no separate binaries ship in the initramfs. What it does have is `wget`,
`udhcpc`, `losetup`, `blkid`, `parted`, `switch_root`.

## Step 0 - what is built

```sh
bash tools/pmos-pmos-initramfs.sh          # boot.img, stock header + patched pmOS init
bash tools/pmos-inject-bootimg.sh \
     out/pmos-final.zip \
     out/boot-pmos-final.img \
     out/pmos-tmp.zip && mv out/pmos-tmp.zip out/pmos-final.zip
```

`pmos-final.zip` carries three separate fixes and all three are verified
inside it:

| what | value |
|---|---|
| `chroot/install_options` | `INSTALL_PARTITION='userdata'` |
| `pmos_install_functions:70` | `findfs PARTLABEL="boot_a"` |
| `./boot/boot.img` in `rootfs.tar.gz` | `header_version=3`, our kernel by md5 |

`userdata` is 239 GB and is not slotted, so it is the only sane install target:
the installer repartitions it into `pmOS_boot` and `pmOS_root`. `boot_a` is
128 MiB, far too small for a 1.5 GB rootfs.

The boot image inside the zip must be built from the **stock** boot image.
pmbootstrap's own image gets `header_version=0`, `page_size=4096`, and this
bootloader refuses it silently. Both images that work here are `magiskboot
repack` of stock and carry `header_version=3`, `page_size=0`.

## Step 1 - put recovery back

```sh
fastboot devices
fastboot flash boot_a ~/kernel_xiaomi_stone/stock/boot.img
fastboot reboot recovery
```

**Verify before going on.** `fastboot flash` on this device has reported `OKAY`
several times while leaving the partition untouched, and once failed with
`Status read failed`. Once recovery is up, read the partition back - from
recovery use `dd`, not `fastboot flash`:

```sh
adb shell 'dd if=/dev/block/by-name/boot_a of=/tmp/chk.bin bs=4M count=32 2>&1 | tail -1'
adb pull /tmp/chk.bin /tmp/chk.bin
md5sum ~/kernel_xiaomi_stone/stock/boot.img /tmp/chk.bin
```

Both must match. `adb exec-out dd if=/dev/block/...` is **not** usable: it
returns plausible data and it is wrong, two reads of the same partition
disagree. Build the file on the phone, pull it, and pull twice.

## Step 2 - sideload the install

```sh
adb sideload ~/lindroid-kernel/out/pmos-final.zip
```

In recovery: Advanced -> ADB Sideload -> swipe to start, then run the command
above.

## Step 3 - verify the install before rebooting

This is the step that was skipped last time, and skipping it is what made the
failure look mysterious.

```sh
# установщик должен был записать наш boot.img в boot_a
adb shell 'dd if=/dev/block/by-name/boot_a of=/tmp/b.bin bs=4M count=32 2>&1 | tail -1'
adb pull /tmp/b.bin /tmp/b.bin
md5sum /tmp/b.bin ~/lindroid-kernel/out/boot-pmos-final.img

# и создать файловые системы в userdata
adb shell 'dd if=/dev/block/by-name/userdata of=/tmp/u.bin bs=4M count=64 2>&1 | tail -1'
adb pull /tmp/u.bin /tmp/u.bin
python3 - <<'EOF'
import struct
d = open('/tmp/u.bin','rb').read()
print('MBR:', d[510:512].hex())
for i in range(4):
    e = d[446+i*16:462+i*16]
    if e[4]:
        lba, cnt = struct.unpack('<II', e[8:16])
        print(f'  p{i+1}: start={lba} секторов={cnt}')
print('метки pmOS:', d.count(b'pmOS_root'), d.count(b'pmOS_boot'))
EOF

# если есть pmos.log - он ответит на вопрос "почему dd ушёл в пустую переменную"
adb pull /tmp/postmarketos/pmos.log /tmp/pmos.log 2>/dev/null && tail -30 /tmp/pmos.log
```

What must be true: the `boot_a` md5 equals our image, and both `pmOS_root` and
`pmOS_boot` are found in `userdata`. If the md5 does not match, the installer's
`dd` failed again - the log will say why, and nothing is lost, because Android
still has not run.

## Step 4 - straight into pmOS

```sh
adb reboot
python3 tools/lind-console.py 180
```

No reboot into Android at any point between here and a working pmOS.

## Step 5 - what to expect on the console

`/dev/ttyACM0` appears a few seconds in. Output is emitted one line at a time
with a pause, because a single `exec >/dev/ttyGS0` followed by ordinary commands
kills the phone - it reboots about three seconds after the gadget comes up.
Two one-byte writes are fine, and twelve kilobytes line by line is fine.

If the phone sits on the logo without a port, the kernel still starts but the
gadget did not. If it reboots in a loop, something wrote to the port in bulk.

To power the phone off from the shell, once our init is running:

```
$ echo off | python3 tools/lind-console.py 10
```

There is no `fastboot poweroff` - the protocol defines only `reboot`. From
fastboot the alternative is a ten second press of Power.

## If it has to be redone

Nothing here is destructive to the boot chain, and slot `b` still holds a
working Android. To get back: flash `stock/boot.img` to `boot_a`, and recovery
comes back, which is also how the phone is recovered from a bad initramfs.

The one thing to keep an eye on is `userdata`: it is the install target, so
every install attempt wipes it.