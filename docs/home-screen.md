# The home screen is Phosh's layout, rebuilt in C

Phosh 0.24.0 (Plasma Mobile) is in Debian bookworm arm64, and its sources are
what the desktop geometry comes from. What is ported is the arithmetic and the
interaction model. What is not ported is the QML, and cannot be: QML is executed
by Qt Quick at runtime, and Qt Quick needs a GPU buffer path — `wl_drm` or
`linux-dmabuf`, `wp_viewporter`, `xdg_popup`, `xdg_decoration`, EGL — none of
which lind implements. Reimplementing it in C is both faster and smaller.

The sources are read from the Debian pool:

    http://deb.debian.org/debian/pool/main/p/phosh/phosh_0.24.0.orig.tar.xz

## The numbers, and where each one comes from

`src/ui/app-grid-button.ui` states the grid formula outright:

    (360px screen width - 2*3px flowbox margins - (4-1)*6px column spacing)
    / 4 columns = 84px

So: 4 columns, 3 dp margin, 6 dp column spacing, 84 dp button, on a 360 dp
screen. This panel is 1080 px wide — which is 360 dp at a density scale of
exactly 3, and 2400 px tall is 800 dp. Every value in the code is `dp(n) = n*3`.
Nothing is guessed.

From `src/ui/home.ui`: the home bar is 40 dp.

From `src/stylesheet/common.css`: type at 9, 13, 14 and 16 dp; corner radii of
6, 8, 9 dp and the pill shape at 9999 dp; padding steps of 3, 4, 6, 8, 9, 12, 16
dp. Tiles use 22 dp, between the 9 dp chip and the pill.

## How it is drawn

Everything goes into the existing scanout buffer through the same path as the
client's own content — the renderer is untouched, as asked.

- **Rounded rectangles, antialiased.** Coverage comes from the distance to the
  corner circle rather than supersampling: one pass, exact enough at these radii.
  Hard corners are also what make the compositor's existing tear obvious, so a
  soft edge is the cheapest way to make its own UI look deliberate.
- **Text** through FreeType over DejaVuSans, which was already in the chroot
  because weston's dependencies pulled it in. Rows are read at `FT_Bitmap.pitch`;
  rounding the glyph width up to a multiple of four instead is one byte off per
  row and shears the glyph into a diagonal smear.

## Apps

From `/usr/share/lindroid/apps`, one per line:

    Имя | команда | цвет

There are no `.desktop` entries here — the chroot has no desktop environment to
register with — so the list is a file rather than a hard-coded array. A built-in
entry keeps the grid from ever being empty, but a launcher that launches nothing
is worse than no launcher, and the file is the part meant to be filled in.

Launching is a double fork with `setsid()`, so the compositor neither waits nor
inherits a dying child. `setsid` also detaches the client from whatever signal
stopped us, which on this phone happens often.

## Known limitation

Any hard-edged UI shows the tear that is already there: `PAGE_FLIP_DONE` arrives
when the commit lands rather than at the vblank, so the swap is immediate. It was
invisible on a full-screen gradient and obvious the moment a 104 px bar appeared
at the top. Rounded corners soften the shapes but do not remove a horizontal
discontinuity. The fix is one line of driver work — complete the flip from the
vblank interrupt instead of the commit path.