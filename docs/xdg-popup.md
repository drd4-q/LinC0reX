# xdg_popup: a NULL in an interface table, and what it looked like

`xdg_popup` is what makes a menu, a dropdown or a context menu appear. It is
also the thing `lind` did not implement for its entire life, while advertising
an `xdg_wm_base` that clients treated as a promise.

This is the story of that gap, because the gap was not a missing feature. It was
a missing line in a table, and it presented as something else entirely.

## The symptom

A client asks for a popup. Nothing appears. The client exits with a diagnostic
that names a protocol it is using correctly:

    lindpop: popup создан, 320x240
    lindpop: configure не пришёл

No protocol error. No disconnect message on the client. The client is simply
never told what size to draw, and since a popup may not draw before its first
`configure`, it waits forever.

The obvious reading is "the compositor does not implement popups". It was doing
something much dumber.

## What was actually wrong

`lind` created the `xdg_popup` object correctly and answered `get_popup`. But
the popup could not exist without an `xdg_positioner`, and the one request that
creates a positioner had no handler:

    static const struct xdg_wm_base_interface wm_base_impl = {
        .destroy = wm_base_destroy,
        .get_xdg_surface = wm_base_get_xdg_surface,
        .pong = wm_base_pong,
    };

`create_positioner` is opcode 1 on `xdg_wm_base`. It is absent. libwayland's
reaction, when a request arrives for an unimplemented opcode, is to log

    listener function for opcode 1 of xdg_wm_base is NULL

**and drop the client.**

So `lind` was not hanging on the popup. It was killing the client one request
earlier and then, correctly according to its own state, waiting forever for a
`configure` that would never come because the client was already dead.

Two things conspired to make this hard to see:

1. The message goes to the **compositor's** log, not the client's. The client's
   only evidence is a wait that never ends.
2. `lindpop` prints `popup создан` the instant the client-side proxy object
   exists. libwayland creates the proxy without a round trip, so that line is
   printed before the compositor has seen anything. It reads as confirmation.

## The rule this taught

The failure mode is general and worth stating plainly:

> Advertising a protocol version implies every request in it. A `NULL` in an
> interface struct is not "unimplemented, tolerated" — it is a client
> disconnect.

There is no way for a client to discover which subset of xdg-shell `lind`
actually implements. The global says version 3, so every well-behaved client
assumes all of it and the compositor is required to agree. Partial
implementation of a protocol is only safe for requests clients are allowed to
skip, and `create_positioner` is not one — the protocol has no way to express
"this popup needs no positioner".

## Two header revisions, one protocol

Worth recording because it nearly sent the diagnosis somewhere false. The
compositor and the test client were built against two generated headers:

    xdg-shell-server-protocol.h   18:43
    xdg-shell-client-protocol.h   18:47

Four minutes apart, and the server's had no `xdg_wm_base_send_done` while the
client's had a `done` listener. The obvious conclusion is a wire mismatch:
opcode 1 sent by the server, unknown to the client. That is a real failure mode
and it was the first thing checked.

It was wrong. Regenerating both from the same XML produced byte-identical files
— same sizes, same content. The header was xdg-shell **version 5**, where
`xdg_wm_base` has exactly one event, `ping`, at opcode 0, and no `done` at all
(`done` was removed after v3). There is no opcode 1 event to mismatch.

Both files had always come from the same XML. The timestamps differed only
because the script writes the two headers sequentially.

Which leaves the log line as the only real evidence, and it is unambiguous once
read as a *server*-side dispatch: "listener" is the request implementation
table, and it is `NULL` at opcode 1. The client never sent an event at all.

## The implementation

`xdg_positioner` is a small object with no drawing and no state that outlives
the popup:

- state: size, anchor rectangle, offset, anchor, gravity
- every `set_*` request writes one field
- `get_popup` reads the whole thing, because by the time a client calls it, all
  the `set_*` requests have already arrived — ordering is the protocol's
  guarantee, not something to be defensive about

`get_popup` asks `xdg_wm_base.create_positioner` only after the object exists, so
there is no allocation that can fail on the wrong side of a resource creation.

### Positioning

The anchor says which point of the *parent's* anchor rectangle the popup
attaches to. The gravity says which point of the *popup* lands there. Nine
combinations each, composed.

The enum values are **not a bit mask**:

    NONE=0 TOP=1 BOTTOM=2 LEFT=3 RIGHT=4
    TOP_LEFT=5 BOTTOM_LEFT=6 TOP_RIGHT=7 BOTTOM_RIGHT=8

`LEFT` is 3, and `3 & 3 == 3`, which is neither 1 nor 2 — so the shift-and-mask
that looks obviously right decodes every edge-only anchor as "none". The
implementation is a switch for that reason, with a comment saying so, because the
arithmetic version is shorter and looks better.

### Constraint adjustment is not optional

A positioner is a *request*. The protocol makes the compositor responsible for
keeping the popup on screen, and a client is entitled to assume it happened.

The test case demonstrates why. Anchor rectangle `(20, 20, 40, 40)`, popup
`320x240`, both anchors `BOTTOM_LEFT`:

    y = (20 + 40) - 240 = -180

The popup is entirely above the top edge of the panel. Rendered honestly, nothing
appears — the client is waiting for input to a menu that was drawn off-screen,
and the failure looks exactly like a compositor that ignores popups.

Full `constraint_adjustment` (flip / slide / resize) is not implemented. Slide
is, because it preserves the client's chosen size, and clamping is applied to the
configure that goes back out, so the client lays itself out where it will
actually be seen rather than where it asked to be.

## Draw order

Popups belong above every toplevel. The surface list is in creation order, which
is not stacking order, so `repaint` walks it twice: non-popups first, popups
second. A single pass renders a menu underneath whatever opened it.

## Dismissal

A tap outside the popup sends `xdg_popup.popup_done` and unmaps it. The hit test
is measured from the popup's own corner (`popup_x`, `popup_y`), not the panel's
— a menu in the middle of the screen that tests against the origin swallows
every tap and dismisses the instant it is used.

Verified both ways in one session: 25 seconds on screen with no input, then a
`popup_done` from a single tap outside.

## Result

    lindpop: popup создан, 320x240
    lindpop: configure получен, 320x240 в (20, 0) панели
    lindpop: 25 с на экране, вышел по таймеру

`(20, 0)` is the clamped truth: x as asked, y pulled up from -180.

## Still missing

- `xdg_decoration` — clients asking for server-side decorations get no answer.
  `gtk4`/`libadwaita` tolerate this; several Qt applications do not.
- `wp_viewporter` — every surface is fullscreen. Any client using a viewport
  draws wrongly or not at all.
- `xdg_toplevel.configure` carries no `states` array, so clients cannot tell
  maximised from fullscreen from dialog.
- Real `constraint_adjustment`, and popups that nest (a menu inside a menu).

Written with AI assistance (OpenCode / Claude). The reasoning, the bugs and the
measurements are from the work itself; see the top-level `README.md` for the
full provenance.
