# vblank does not fire on this device, and what that rules out

The flickering, the frozen animation in weston, and the reason the obvious
driver fix cannot be applied. All three come from one measurement.

## The fix that seemed obvious

A page flip completes at commit time on this driver. `sde_kms_wait_for_commit_done()`
calls `sde_crtc_complete_flip()` as soon as `MSM_ENC_COMMIT_DONE` returns.

That is before the swap. The encoder arms the buffer change for the next frame
boundary, so at commit-done the panel is still scanning out the *old* buffer and
will not read the new one for up to a full refresh period.

A compositor told "your flip is done" starts drawing the next frame immediately.
It is drawing into the buffer the panel is actively reading. The seam lands
wherever in the scanout the draw happened to be. On a 1080x2400 panel at 55.9 Hz
that is up to 17.9 ms of the previous frame being overwritten in place.

The DRM core says as much in `drm_crtc_arm_vblank_event()`:

> Drivers ... need to manually send out the event from their interrupt handler by
> calling `drm_crtc_send_vblank_event()` and make sure that there's no possible
> race with the hardware committing the atomic update.

So: move `sde_crtc_complete_flip()` from the commit path into the vblank
interrupt handler. It compiled. It was flashed. **The screen went black and no
DRM events arrived at all.**

## Why

SDE has a complete vblank chain, and reading it is what makes the failure
comprehensible:

    RD_PTR IRQ  (per frame, hardware)
      -> sde_encoder_phys_cmd_rd_ptr_irq()        sde_encoder_phys_cmd.c:280
      -> sde_encoder_vblank_callback()            sde_encoder.c:3119   handle_vblank_virt
      -> sde_crtc_vblank_cb()                     sde_crtc.c:2438     crtc_vblank_cb
      -> drm_crtc_handle_vblank(crtc)             the core's handler

Nothing is missing from the chain. A correction to an earlier conclusion in this
repository: `drm_vblank_handler()` *is* reached — through
`drm_crtc_handle_vblank()`, not by a direct call, which is why searching for the
function name outside `drm_vblank.c` finds nothing.

The chain is also wired correctly, which took some checking to rule out:

- `struct sde_encoder_virt_ops` has `handle_vblank_virt` first, and
  `sde_encoder_create_phys()` initialises `parent_ops` positionally starting
  with `sde_encoder_vblank_callback`. A mismatched order would have looked
  exactly like this bug and is worth stating because the initialiser is
  positional, not designated.
- `sde_encoder_phys_cmd_is_master()` returns true unless the encoder is a
  pingpong-split slave. It is not, here.
- `sde_encoder_phys_cmd_control_vblank_irq()` registers `INTR_IDX_RDPTR` on the
  first `vblank_refcount` transition and unregisters it only when the count
  returns to zero. The only caller that disables it is
  `sde_kms_vm_pre_release()` — when the DRM master goes away.

So the interrupt is registered, the callback is wired, and it should fire once
per frame.

## The measurement

SDE already exports a counter for exactly this, and it needed no new code:

    struct sde_crtc {
        u32  vblank_cb_count;
        ktime_t vblank_last_cb_time;
        ...
        struct kernfs_node *vsync_event_sf;
    };

`vsync_event_show()` publishes `ktime_get()` of the last vblank callback, and
`sde_crtc_vblank_cb()` writes it on every invocation. Read it twice:

    /sys/devices/platform/soc/5e00000.qcom,mdss_mdp/drm/card0/sde-crtc-0/vsync_event

Result on the phone:

    --- замер 1 ---
    VSYNC=380193921467
    --- замер 2 (через 3 с) ---
    VSYNC=380193921467

Nonzero — so the callback has run at least once. **Unchanged across three
seconds** — so it is not running per frame.

Alongside it:

    measured_fps: fps: 0.0 duration:1000000 frame_count:0

SDE's own frame accounting agrees. The interrupt fires around mode set and then
stops being delivered as a per-frame event.

## What that explains

**The frozen weston animation, which predates this kernel work.** weston
computed a repaint delay of `-805239 msec` and then stopped scheduling frames:

    Warning: computed repaint delay is insane: -805239 msec

weston builds its presentation clock from vblank/sequence events. With the vblank
callback not delivering, the timestamps it gets are meaningless, and it refuses
to schedule. Confirmed independently of any rendering question: weston's CPU time
sat at 3 seconds and did not advance over the following 5, while a 60 fps
animation would have to burn CPU. Both compositor and client were blocked
waiting for a frame callback that could not be scheduled.

**Why `lind` is unaffected.** lind sends frame callbacks on commit, not on flip
completion, so it never depends on the vblank timestamp. It animates. It is also
the only thing here that has ever displayed correctly.

**Why the patched kernel showed nothing at all.** With the flip event deferred to
a callback that does not fire, no flip ever completes, and a compositor waiting
for its first flip presents nothing.

## Consequences

- **Do not complete page flips from SDE's vblank path on this device.** It cannot
  work. The patch was reverted; the tree is back at `32770983e`.
- **The flicker is not fixable by moving the notification.** The idea was right —
  vblank is the correct instant, and the core recommends delivering from the
  interrupt handler — but this driver does not deliver vblank, so there is no
  correct instant available to deliver at.
- **Any compositor that depends on accurate presentation timing will stall here.**
  That is weston, and it would be sway, and it is not a lind defect. It is a
  property of this kernel and this panel path.

## What would actually fix it

Not a one-line change. In rough order of size:

1. **Make the RD_PTR interrupt deliver per frame.** Whatever stops it after the
   first frame — idle-mode IRQ gating, `frame_trigger_count` bookkeeping in
   `sde_encoder_phys_cmd_rd_ptr_irq()`, or the refcount asymmetry between
   `drm_crtc_vblank_on()` (called from `sde_kms_vm_primary_prepare_commit()` on
   every commit) and `drm_crtc_vblank_off()` (called only from
   `sde_kms_vm_pre_release()`) — has to be found and fixed. That refcount
   asymmetry is already suspicious: it grows by one per commit, and the only
   decrement is on master release.

2. **Use the DSI TE signal instead.** `phys_enc->has_intf_te` exists, and SDE
   already waits on it for panels that need a frame before backlight. TE is a
   per-frame hardware signal that is not the same thing as the RD_PTR interrupt
   and may well still be live. This is the more promising route: it is the signal
   that means "the panel has started reading this buffer".

3. **Stop needing it.** A compositor that draws faster than the panel and never
   changes a hard edge does not tear. This is a real option for a phone shell:
   it is what `lind`'s home screen already does by not animating.

## Honest note on the mistake

The patch was written from a reading of the code, built, and flashed without
being able to test it on the target. The chain looked complete, so the missing
piece was assumed to be a logic error rather than absent hardware delivery. It
was the second kind, and one sysfs read would have said so before the flash.

Written with AI assistance (OpenCode / Claude).
