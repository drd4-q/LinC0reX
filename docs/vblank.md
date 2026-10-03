# vblank: not delivered, and then delivered

The flickering, the frozen animation in weston, and a driver fix that could not
be applied. All three came from one measurement, and the fix turned out to be a
device tree property rather than the code it was assumed to be.

**This document was written twice.** The first version concluded that SDE's
vblank interrupt does not fire on this device and that the flip patch was
therefore impossible. That conclusion was correct as far as it went and wrong
in its conclusion: the interrupt was not firing because the panel's TE was
routed away from the DSI link, and fixing the routing made it fire at frame
rate. The sections below are kept as written, with the correction at the end,
because the path to the answer is more useful than the answer alone.

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

## Correction: the interrupt was silent, not absent

Every conclusion above rests on `vsync_event` not changing. That was taken to
mean the callback never ran after the first time. It did run — just not
usefully.

The panel's TE never reached the driver at all. `sde_encoder_phys_cmd_init()`
sets `has_intf_te` from the hardware catalog, which is true for DSI, so
`INTR_IDX_RDPTR` is registered as:

```c
    if (phys_enc->has_intf_te)
            irq->intr_type = SDE_IRQ_TYPE_INTF_TEAR_RD_PTR;   /* from INTF */
    else
            irq->intr_type = SDE_IRQ_TYPE_PING_PONG_RD_PTR;   /* from PINGPONG */
```

And `dsi_setup_trigger_controls()` was configured to keep TE off the DSI link:

```c
    if (cfg->te_mode == DSI_TE_ON_EXT_PIN)
            reg |= BIT(31);      /* do not look for TE on the link */
    else
            reg &= ~BIT(31);
```

`te_mode` came from the panel device tree, and the panel has no
`qcom,mdss-dsi-te-pin-select`, so `dsi_panel_parse_host_config()` fell back to
`1` — which is `DSI_TE_ON_EXT_PIN`. Across the 70 panels in this tree, 38 set
`1` explicitly, **none** set `0`, and 32 say nothing and inherit the fallback.
Nothing is ever routed from the link, so the INTF TE status is never set and the
RD_PTR interrupt cannot fire.

The GPIO in the panel's node, `tlmm 23`, is not an alternative path. Its handler
says what it is for:

```c
    /*
     * This irq handler is used for sole purpose of identifying
     * ESD attacks on panel ...
     */
    complete_all(&display->esd_te_gate);
```

Two changes to the panel device tree, on both m17 panel nodes since Android's
runtime choice between them is not observable from userspace:

- `qcom,mdss-dsi-te-pin-select = <0>` — TE from the DSI link
- DCS `0x35` (TEON) and `0x39` (TESCANLINE) in `qcom,mdss-dsi-post-panel-on-command`,
  because nothing in the original `on-command` ever asked the panel to assert TE

After that:

    vsync_event:  +0.99..1.01 s per one-second read, tracking real time
    /proc/interrupts  163: msm_drm  +363 over 3 s = 121.0/s

121 interrupts per second is the panel's frame rate. The signal exists.

### Two measurement errors worth keeping

**Reading `vsync_event` cannot tell you the rate.** It publishes
`ktime_get()` of the last callback, so it tracks real time whether the callback
runs at 1 Hz or 120. An early conclusion here — that TE fired "about once a
second" — was an artefact of sampling through `adb shell`, where each loop cost
about a second rather than the 400 ms intended. The rate came from
`/proc/interrupts`, and it is 121/s.

**`sde_crtc.vblank_cb_count` would have answered it directly**, and it is
already incremented in `sde_crtc_vblank_cb()`. It is simply not exposed; only
the timestamp is.

## Status

The flip patch is re-applied, and it now has a bound on the wait it depends on.
`msm_drm` has also been seen falling silent for stretches around mode changes,
and `sde_crtc->event` is a single slot — a second mark would overwrite the first
and the first client would never hear anything. So if a flip is still pending
when the next commit lands, its vblank never came and the event is sent
immediately. A client waits at most one extra frame instead of forever.

weston still does not animate with vblank present. That is a separate problem:
the signal is there and weston is not using it, which is a question about weston
rather than about this driver.

## What it took

Kept from the original draft, with what actually happened marked. Worth having
both, because the first two candidates were wrong guesses and the third was
right.

1. ~~Make the RD_PTR interrupt deliver per frame.~~ Wrong theory. Idle-mode IRQ
   gating and the `frame_trigger_count` bookkeeping in
   `sde_encoder_phys_cmd_rd_ptr_irq()` were not involved. The
   `drm_crtc_vblank_on()` / `drm_crtc_vblank_off()` refcount asymmetry — the
   former called from `sde_kms_vm_primary_prepare_commit()` on every commit, the
   latter only from `sde_kms_vm_pre_release()` — is real but harmless here: an
   inflated refcount keeps the interrupt enabled, which is what we want.

2. ~~Use the DSI TE signal instead.~~ Right idea, wrong level. `has_intf_te`
   already existed and SDE already used it; the signal was simply never routed
   there. No code change was needed for this at all.

3. **Ask the panel for TE, and read it from the right place.** One device tree
   property and two DCS commands. See the correction above.

## Honest note on the mistake

The first attempt at this patch was written from a reading of the code, built,
and flashed with no way to test it on the target first. The chain looked
complete, so the missing piece was assumed to be a logic error rather than a
signal that never arrived. It was the second kind, and one sysfs read would have
said so beforehand.

Worse, the conclusion drawn afterwards — "do not complete page flips from SDE's
vblank path on this device, it cannot work" — was wrong, and was the kind of
wrong that closes a door. The interrupt was silent because a device tree
property was missing, not because the hardware could not produce it. Two
properties and two DCS commands later it runs at 121 interrupts per second.

The general lesson is not about device trees. It is that "this cannot be done"
and "this is not being done" look identical from userspace, and the difference
between them is one measurement. Making that measurement before writing the
patch would have cost a minute. Making it after cost a flash, a black screen,
and a conclusion that was confidently inverted.
