# Why PAGE_FLIP_DONE never arrives

Measured, not inferred. The first version of this note claimed page flip worked,
based on `vblanktest`. That was wrong: the `seq` values it printed were garbage
and identical on all three runs, which was the signal, and it was not followed
up. The real numbers came from instrumenting the compositor to log the event type
rather than just counting events.

## The measurement

```
lind: drm event type=2 len=32 seq=406503840272 time=0
lind: repaint=3136 frames=3136 events=1 read_err=0 flip_err=0 dropped=3135 pending=1
```

One event in 3136 flip requests. Type 2 is `DRM_EVENT_VBLANK_2`, not
`DRM_EVENT_PAGE_FLIP_DONE` (4), and `time=0`. The requests themselves were all
accepted — `flip_err=0`. So the ioctl succeeds, the hardware is programmed, and
the completion never comes back.

## The cause

`drm_crtc_send_vblank_event()` is never reached, because `sde_crtc->event` is
always NULL when `sde_crtc_complete_flip()` runs. Nothing ever fills it in for a
flip.

Two independent faults on the same path, in
`techpack/display/msm/sde/sde_kms.c` and `sde/sde_crtc.c`:

**1. The commit that caches the event is filtered out for flips.**

`sde_crtc->event = crtc->state->event` lives in `_sde_crtc_atomic_commit()`,
reached via `sde_crtc_enable()` from the core's
`drm_atomic_helper_commit_modeset_enables()`:

```c
/* Need to filter out CRTCs where only planes change. */
if (!drm_atomic_crtc_needs_modeset(new_crtc_state))
        continue;
```

and in kernel 5.4, `include/drm/drm_atomic.h:974`:

```c
static inline bool
drm_atomic_crtc_needs_modeset(const struct drm_crtc_state *state)
{
	return state->mode_changed || state->active_changed ||
	       state->connectors_changed;
}
```

A page flip changes only the plane. All three are false. The commit never runs.

**2. It reads the wrong state even when it does run.**

`page_flip_common()` sets the event on the **new** state:

```c
crtc_state = drm_atomic_get_crtc_state(state, crtc);
...
crtc_state->event = event;
```

`_sde_crtc_atomic_commit()` reads `crtc->state->event`, which is still the **old**
state until `drm_atomic_helper_commit_hw_done()`. State swap happens at the end
of `msm_atomic_commit_tail()`, after the point where the event is needed.

## The fix

Branch `fix/page-flip-completion` in `~/dm-kernel`, commit `32770983e`.
`sde_kms_cache_flip_events()` reads the event from the state actually being
committed, in `sde_kms_commit()`.

Ordering is what makes that the right seam. From `msm_atomic.c`, one commit runs:

| step | call |
| --- | --- |
| 3 | `drm_atomic_helper_commit_planes` |
| 4 | `drm_atomic_helper_commit_modeset_enables` ← old event store, skipped for flips |
| 5 | `kms->funcs->commit` ← **patch lands here** |
| 6 | `msm_atomic_wait_for_commit_done` → `sde_crtc_complete_flip()` |
| 7 | `kms->funcs->complete_commit` ← too late |
| 9 | `drm_atomic_helper_commit_hw_done` ← state swap |

Step 5 runs on every atomic commit including a flip, and still precedes the flip
completion at step 6. A modeset is unaffected: the old path caches the old
state's event, which is NULL, and the new path fills it in.

The event is deliberately not moved onto the vblank path. The encoder already
schedules the buffer swap at the frame boundary in command mode; this only tells
the client the commit landed.

Verified to compile: `CC techpack/display/msm/sde/sde_kms.o`, clean.

## What this was blocking

**weston.** Its one-frame stall has the same cause: with pixman rendering weston
presents through a page flip, the completion never arrives, the repaint loop
waits forever, and the single frame it managed to present is the only one. Not
weston bookkeeping — a driver fact.

**Tearing.** With one buffer drawn in place, the compositor writes what the panel
is scanning out. Frame pacing cut the visible flicker substantially (the flicker
report improved and CPU went from 70.9% to 2.6%), but no rate limit removes the
tear. Double buffering needs the flip.

## Not in this change

The DSI path itself was checked and is not implicated: `mdp_transfer_time=0` in
`dsi_display_set_mode` looked suspicious, but mode sets only happen at startup
and once at 60 Hz for a single 200 ms window, so the panel is not being re-set
during rendering. The vblank IRQ is not the problem either — the commit-done
event the encoder does report is what step 6 waits on, and it is step 5 that
never runs.