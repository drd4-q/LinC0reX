# The GPU question, answered with an ioctl

`wl_drm` and `linux-dmabuf` were the last things standing between `lind` and
Qt Quick, and therefore between `lind` and Phosh. They were also the things
everyone would assume are worth implementing.

This is the measurement that says otherwise, and it is worth recording because
the first version of it produced the exact opposite answer.

## What was being asked

Not "is there a GPU" — obviously there is, it is an Adreno 619 driving the panel.
The question is narrower and it decides the architecture:

> Can a userspace OpenGL/GLES stack — Mesa, and through it EGL, Qt Quick,
> GTK's renderer, SDL — ever allocate a buffer on this device?

If yes, `lind` must eventually implement `wl_drm` and hand the card fd to
clients. If no, software rendering is permanent and the answer is to write
efficient clients, not to port ones.

## The probe

`tools/drmprobe.c` opens a DRM node and asks six questions in order, reporting
each separately, because "it failed" is useless and "it failed at exactly this
call" is the whole answer:

1. `open` — is the node reachable
2. `drmGetVersion` — which driver claims it
3. `drmModeGetResources` — does it speak KMS
4. `DRM_CAP_DUMB_BUFFER` — the capability bit Mesa probes for
5. `create_dumb` + `map_dumb` + `mmap` + a write — can userspace actually get
   memory it can touch

Step 5 is the only one that settles it, and it exists because a capability bit
can be present while the ioctl behind it refuses.

## First run: a confident wrong answer

    4. DRM_CAP_DUMB_BUFFER                ДА - Mesa сможет выделять через dumb
    6. create_dumb 32x32                  НЕТ - Invalid argument

`create_dumb` returns `EINVAL`. Read naively that says the driver will not
allocate memory, no Mesa, no GPU path, ever — and it is a hardware-shaped
conclusion drawn from a bug in the probe.

`EINVAL` from a DRM ioctl is the standard reply for a malformed request, and a
DRM ioctl number is `_IOWR` over the **size of its argument struct**. So either
the struct was wrong or the ioctl number derived from it was.

It was. The first version declared the modern `drm_mode_create_dumb`:

    width, height, pixel_format, bpp, flags, handle, pitch, size   /* 40 bytes */

The phone's kernel — Darkmoon-Reborn 5.4,
`include/uapi/drm/drm_mode.h` — declares the **old** one:

    height, width, bpp, flags, handle, pitch, size                 /* 32 bytes */

No `pixel_format`, and `height` before `width`. Eight bytes of difference makes
a different ioctl number, so the kernel received a request it did not recognise
and said `EINVAL`, which is exactly what it says to any client that got the ABI
wrong.

Both structs are "the" struct. The DRM dumb ABI changed once, long ago, and
there is no negotiation: the caller must know which kernel it is talking to.

The chroot's own libdrm headers declare the old shape, which is another way of
seeing that this phone is pre-ABI-change.

## Second run: the answer

    драйвер: msm_drm 1.4.0
    DRM_CAP_DUMB_BUFFER                ДА
    create_dumb 32x32                  ДА - буфер выделен
    map_dumb                           ДА
    mmap буфера                        ДА
    запись в буфер                     ДА

Memory works. So the naive conclusion flips to "the GPU path is possible" — and
that is also wrong, for a different reason.

## What the answer actually means

`drmGetVersion` reports **`msm_drm`, version 1.4.0**. That is SDE: the display
driver that scans out the panel. It is not `msm_kgsl`.

KGSL is the GPU, and it lives in `drivers/gpu/msm/kgsl_*.c` as a separate
module, `msm_kgsl.ko`. It is not a DRM driver, so it creates no DRM node,
registers no `DRM_CAP_*`, and answers no ioctl. Nothing in userspace can reach
the Adreno through DRM, because the thing on `/dev/dri` is not the thing that
talks to the GPU.

So the two facts combine like this:

- **Buffers are allocatable** — because that is how a KMS-only driver allocates
  framebuffers. It is system RAM, and it is exactly what `lind` already uses.
- **There is no GPU behind those buffers** — Mesa on this node would find a
  driver that renders nothing, and would fall back to `swrast`, which allocates
  the same system RAM and draws on the CPU.

Which is `lind` already doing, but with a smaller program and a smaller memory
footprint than a full GL stack.

## Consequences

**Do not implement `wl_drm` or `linux-dmabuf` in `lind`.** It is the single
largest piece of remaining protocol work — buffer queues, fences, sync objects,
dmabuf import from clients — and it buys nothing. A client that gets a `wl_drm`
buffer here gets a CPU memory buffer with extra steps, and still has to
software-render into it.

This also explains the earlier observation that weston's GL backend failed, and
explains it better than "no `DRI_DRI2` for SDE" did: there was never a GL driver
to load. glvnd searched, found no vendor ICD, because no ICD registers a DRM
node on this kernel. Not a packaging problem, not a permission problem.

**Phosh, Plasma Mobile, Qt Quick are not reachable on this kernel.** Not because
of missing protocols, and not because of missing packages — `plasma-mobile`
5.27.2 installs cleanly on bookworm arm64 with all 56 direct dependencies
resolving. They need a GPU that this kernel does not expose to userspace.

The same is true of `weston` with its GL or pixman renderer, and of anything
built on SDL's accelerated path.

**What remains reachable is software.** A `weston-terminal` with the pixman
renderer, `foot` if built against a freestanding toolkit, or — the realistic
option — clients written against `lind`'s actual cost model, where every full
panel repaint is a CPU blit and has to be budgeted against an 8.3 ms refresh
period. That budget is why `lind` uses in-place single-buffer rendering, and why
the 31 fps banded renderer was abandoned rather than tuned.

## What would change the answer

Only the kernel, not the compositor:

- `msm` (the in-tree freedreno driver) built as a second DRM driver for the same
  GPU. It cannot coexist with SDE on `card0` — both would be DRM drivers for one
  device — so it needs a different device assignment.
- Or KGSL exposed through a DRM render node, which no existing kernel does.
- Or SDE taught to proxy render, which is not a thing.

None of those are in scope for a userspace project, and all of them are the
reason this file exists: to record that the dead end is in the kernel, was
measured rather than assumed, and should not be re-litigated from the
compositor side.

Written with AI assistance (OpenCode / Claude).
