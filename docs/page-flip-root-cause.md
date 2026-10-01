# Page flip on this driver: what was actually wrong

## Correction first

The first version of this note concluded that the driver never sends a page flip
completion, and pointed at two faults in `sde/sde_kms.c` as the cause. The
first half of that is wrong, and it was wrong in a way that should have been
caught sooner.

What was measured at the time:

```
lind: drm event type=2 len=32 seq=406503840272 time=0
lind: repaint=3136 frames=3136 events=1 flip_err=0 dropped=3135 pending=1
```

One event in 3136 flip requests. That reads as a driver that never completes a
flip. It is not. **The compositor was matching the wrong event type.**

This kernel stamps a flip completion as `DRM_EVENT_FLIP_COMPLETE`, value 0x02 —
`drivers/gpu/drm/drm_atomic_uapi.c:906`, in `drm_mode_page_flip()`. lind compared
against `DRM_EVENT_PAGE_FLIP_DONE = 0x04`, the value newer kernels renamed it to.
The `type=2` in that log *was* a flip completion. It arrived, was discarded as
unrecognised, `flip_pending` never cleared, and the compositor therefore never
issued a second flip — so all 3135 later frames were dropped.

One frame presented and one event logged look identical from the outside to a
driver that never answers. The discriminating test was never run.

With the constant fixed:

```
lind: drm event type=2 seq=371177573128      (repeating)
lind: repaint=1313 frames=1079
```

1079 frames presented. Page flip works.

## The second wrong signal

`vblanktest` was cited as showing flip events arriving with an identical
"garbage" `seq` on all three runs, and the note records that this looked wrong
at the time. It was wrong, and following it up would have avoided the whole
detour.

Those three events were the vblank events from the test's own
`drmWaitVBlank()` calls, read with a `struct drm_mode_page_flip_event`. Same
struct mismatch, opposite direction. The vblank clock itself was always sound
and is what makes scan-synchronised rendering possible:

```
#0 seq=195  tval_usec=661430
#1 seq=196  tval_usec=669773    +8343 us
#2 seq=197  tval_usec=678123    +8350 us
```

120 Hz, period 8343 us, timestamp in CLOCK_MONOTONIC. That part was never in
question.

## What is true about the driver

The reasoning about the event-caching path holds, even though it was not the
cause of the observed stall:

- `sde_crtc->event = crtc->state->event` in `_sde_crtc_atomic_commit()` is
  reached via `sde_crtc_enable()` from
  `drm_atomic_helper_commit_modeset_enables()`, which skips any CRTC where
  `drm_atomic_crtc_needs_modeset()` is false — and that is
  `mode_changed || active_changed || connectors_changed`, all of them false for a
  page flip. The core's own comment there reads "Need to filter out CRTCs where
  only planes change."
- The same code reads `crtc->state->event`, which is still the old state until
  `drm_atomic_helper_commit_hw_done()`. The event was set on the new one, by
  `page_flip_common()`.

So the patch in `~/dm-kernel` branch `fix/page-flip-completion` (commit
`32770983e`) fixes a real latent bug: reading the wrong state, and caching it on
a path flips do not take. It compiles.

**Whether it is needed is not established.** The only build it could be tested
against had already been corrected in userspace, and no A/B against the unpatched
kernel was run. The 1079 frames above were produced with the patch in place and
with the constant fixed at the same time, so they do not separate the two.

Settling it would mean flashing the unpatched kernel and re-running, which costs
another flash and buys little: double buffering works either way now, and the
patch is a correctness fix to an event-caching path rather than a workaround.

## Ordering of the commit, for the patch

From `msm_atomic.c`, useful if this is ever revisited:

| step | call |
| --- | --- |
| 3 | `drm_atomic_helper_commit_planes` |
| 4 | `drm_atomic_helper_commit_modeset_enables` — old event store, skipped for flips |
| 5 | `kms->funcs->commit` — patch lands here |
| 6 | `msm_atomic_wait_for_commit_done` → `sde_crtc_complete_flip()` |
| 7 | `kms->funcs->complete_commit` — too late |
| 9 | `drm_atomic_helper_commit_hw_done` — state swap |

## Frame pacing, which was a real bug and remains

Independent of the above, lind used to repaint on every client commit. A client
committing at ~800 Hz got its frame callback straight back and re-committed, so
the buffer was rewritten faster than the panel read it. Two distinct faults:

- `wl_event_source_timer_update()` on an already-pending timer pushes the
  deadline out again, so the frame was postponed forever: 13 repaints against
  28004 client frames.
- A timer callback returning 0 is *removed* from the loop, so one repaint for
  the life of the process.

Fixed by arming only when not pending and returning 1. Compositor CPU went from
70.9% to 2.6% for the same job.

## Check the event type before blaming the driver

The lesson worth keeping, since it cost a kernel patch and a flash: when a DRM
event "never arrives", log its type and compare it against what *this* kernel
sends. `DRM_EVENT_VBLANK` / `FLIP_COMPLETE` / `CRTC_SEQUENCE` were 0x01/0x02/0x03
here; the modern tree renumbers to `VBLANK_1`/`VBLANK_2`/`PAGE_FLIP`/
`PAGE_FLIP_DONE`. A userspace built against newer libdrm headers against an older
kernel is exactly where that mismatch lives.