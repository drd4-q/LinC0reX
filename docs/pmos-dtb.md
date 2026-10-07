# Where the device tree actually comes from

Measured 2026-10-04 on the phone, `adb shell` while running
`Darkmoon-20261003-0715`, active slot `_a`.

## Answer

The bootloader passes it. Not the kernel, not `boot.img`, not `dtbo`.

Two partitions on the UFS LUN that also holds `boot`:

| what | partition | device | size |
|---|---|---|---|
| base device tree | `vendor_boot_a` | `/dev/block/sde18` | 96 MiB |
| overlay, index 0 | `dtbo_a` | `/dev/block/sde13` | 24 MiB |
| kernel + ramdisk | `boot_a` | `/dev/block/sde9` | 128 MiB |

The base tree is an FDT at offset `0x2000` of `vendor_boot_a`, 391685 bytes.
The partition starts with the magic `VNDRBOOT`. The overlay in `dtbo_a` is a
Qualcomm dt_table, magic `0xd7b7ab1e`, two entries of 461 bytes each, and both
are overlay fragments - `/fragment@0/__overlay__` plus `/__fixups__ { soc }` -
not trees.

The running kernel sees the merged result:

```
$ adb exec-out cat /sys/firmware/fdt | wc -c
393908
```

393908 against a 391685-byte base plus a 461-byte overlay: the bootloader merged
them, which is what `androidboot.dtb_idx=0` and `androidboot.dtbo_idx=0` in
`/proc/cmdline` are for.

## A factory reset cannot take the tree away

This was the open question, and the answer is the safe one. `vendor_boot_a` and
`dtbo_a` are **physical partitions of `sde`**, the same LUN as `boot`. They are
not logical volumes inside `super`, which is a different device entirely:

```
/dev/block/by-name/vendor_boot_a -> /dev/block/sde18
/dev/block/by-name/dtbo_a        -> /dev/block/sde13
/dev/block/by-name/super         -> /dev/block/sda8
```

So the name `vendor_boot` is misleading. On devices where it *is* a logical
volume, this question would have had the opposite answer and the pmOS image
would have had no device tree at all after a reset, failing as a hang rather
than as a missing file. Here it does not apply.

## What is not carrying a tree

Measured, because each of these looked like a candidate:

- **our `Image`** - zero FDT blobs, `\xd0\x0d\xfe\xed` does not occur
- **the stock kernel**, unpacked from `stock/boot.img` - zero
- **the flashed `boot_a`**, dumped off the phone - zero
- **builtin DTB** - in `out/System.map`, `__dtb_start` equals `__dtb_end`, so it
  is zero bytes long; no `CONFIG_OF_BUILTIN` either
- **`dtbo_a`** - overlay fragments only, and `CONFIG_OF_OVERLAY` is not set in
  our kernel, so the kernel could not have applied them in any case

Beware of this false positive: grepping a kernel for a DT property name hits,
and proves nothing. `qcom,mdss-dsi-te-pin-select` is a C string literal in
`techpack/display/msm/dsi/dsi_panel.c:1122`, so it is in every build of this
tree. Only `__dtb_start`/`__dtb_end` and the FDT magic settle it.

## The panel TE patch is not on the phone

This corrects an earlier conclusion in this project, which credited the 121/s
vblank to the panel DT patch. It was the kernel side.

Walking the device tree property by property:

| tree | `te-pin-select` in m17 panels |
|---|---|
| our built `moonstone.dtb` | `= 0` - our patch, in both m17 nodes |
| `vendor_boot_a` base DTB | **absent** |
| live `/sys/firmware/fdt` | **absent** |

In the live tree only 8 nodes carry the property, all other panels
(`rm69299_visionox_amoled`, `visionox_r66451`, `sharp_qsync`), all with the
stock value `1`. The two m17 nodes - `qcom,mdss_dsi_m17_38_0c_0a_fhdp_dsc_vid`
and `qcom,mdss_dsi_m17_k6s_38_0c_0a_fhdp_dsc_vid` - do not have it at all, so the
driver falls back to `1`, `DSI_TE_ON_EXT_PIN`, exactly the condition that was
blamed for silent vblank. And vblank fires at 121/s anyway.

So the patch was never flashed, or was overwritten, and whatever made vblank
work was in the kernel tree. The panel DT patches remain correct in the source
and are worth keeping, but they are not what is running.

Nothing depends on fixing this for pmOS. The image gets the same tree Android
gets today, which is a tree vblank already works with. Flashing our dtb into
`vendor_boot` would be a separate experiment, on a partition that currently
works, with no upside for a console boot.

## What this means for the pmOS image

Nothing to do. `boot.img` should be kernel plus initramfs with no `dtb` slot and
no appended tree, which is what `deviceinfo_dtb` being off and
`pmos-lind-kernel.sh` produce. The bootloader supplies the tree from
`vendor_boot_a` regardless of what is in `boot.img`, exactly as it does for
Android, and our SDE kernel is the same kernel that drives that tree today.