# Licensing

Short version: everything here is **GPL-2.0-only**, and that is the licence the
kernel requires. There is no GPL-3.0 code in the tree, and adding any would
have been the actual violation.

## What each part is licensed under

| Part | Licence | Where it is declared |
|---|---|---|
| `drivers/lindroid-drm/` | GPL-2.0-only | `SPDX-License-Identifier` in 13 source files, `MODULE_LICENSE("GPL v2")` |
| `tools/*.sh`, `tools/*.py`, `install.sh`, `build-zip.sh` | GPL-2.0-only | `SPDX-License-Identifier` at the top of each file |
| `patches/*.patch` | see "Gap" below | nothing yet |
| `~/dm-kernel/LICENSE` | GPL-2.0 (June 1991) | the base tree this is built on |

The project's own `LICENSE` is GPL-2.0.

## Why GPL-2.0 and not GPL-3.0

This is worth being precise about, because the intuition runs the other way.

`drivers/lindroid-drm/` is compiled **into the kernel**. The kernel is
GPL-2.0. GPL-2.0 and GPL-3.0 are **not compatible** for combined works: the
FSF has never granted permission to combine a GPL-2.0-only work with a
GPL-3.0-only work, and the resulting binary must be treated as GPL-3.0, which
the GPL-2.0-only parts of the kernel would then be violating.

So had the driver been GPL-3.0-only, **linking it into the kernel would have
been the licence violation**, not the other way round. GPL-2.0-only is the
correct and compatible choice, and it is what the code says.

`MODULE_LICENSE("GPL v2")` is a kernel-module licence tag, not the licence of
the module. It declares the module's licence to the kernel's
`__license`/taint machinery. The `SPDX-License-Identifier` lines are the
authoritative statement, and all of them say GPL-2.0-only.

## Attribution of derived work

`drivers/lindroid-drm/` descends from DisplayLink's `evdi`. The copyright
notices are intact and were not stripped:

```
Copyright (c) 2015 - 2020 DisplayLink (UK) Ltd.
Copyright (c) 2025 Lindroid Authors
```

GPL-2.0 §2(a) requires that modified versions carry prominent notices of
change and retain the original notices. Both are present. The Red Hat
`Copyright (C) 2012` line in the same tree comes from the DRM core code the
driver builds on, likewise retained.

## What the boot image actually contains

The boot image we ship is **not** a repackaged OEM kernel. It is:

- the **header** taken from the stock image (a few hundred bytes of metadata:
  page size, header version, partition sizes — no code), because this
  bootloader refuses anything that is not `header_version=3, page_size=0`
- **our** `Image`, built from this tree
- **our** pmOS initramfs

So the binary we distribute corresponds to source we publish here. That
satisfies GPL-2.0 §3 for the boot image.

## What the recovery zip redistributes

The zip carries a pmOS root filesystem. It is a stock Alpine/pmOS userland
with three edits, none of which touch licensed code:

- `./boot/boot.img` — replaced with our image
- `./etc/inittab` — one line appended (a `respawn` entry)
- `./etc/shadow` — the second field of the `root` line cleared

Package licence metadata in `APKINDEX` is untouched, and no upstream licence
file is removed or altered. pmOS's own scripts are MIT and their headers stay
in place; MIT permits modification, and the only requirement is retaining the
notice.

## Gap to close before publishing

`patches/*.patch` are bare diffs with no commit message, no author line and no
licence statement. For a repository that people are expected to build from,
they should be regenerated as proper `git format-patch` output:

```
cd ~/dm-kernel
git format-patch -o ~/lindroid-kernel/patches/ <base>..HEAD
```

Each such patch should carry `Signed-off-by:` and the commit message should
state the licence. Until then, the licence of the patched tree is only
implicit in the base repository's `LICENSE`.

A `README` note stating the licence and where to obtain the corresponding
source would also be worth adding, since GPL-2.0 §3 requires offering the
source when distributing binaries.

## Summary

- No GPL-3.0 code is present, and none should be added to anything that is
  linked into the kernel.
- The driver is GPL-2.0-only and compatible with the GPL-2.0 kernel it is
  compiled into.
- DisplayLink's notices are retained.
- The boot image we ship contains our own kernel, whose source is in this
  repository.
- `patches/*.patch` need proper headers before publication.
