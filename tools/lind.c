// SPDX-License-Identifier: GPL-2.0-only
/*
 * lind - a minimal Wayland compositor for this phone's display.
 *
 * weston runs on this panel, finds the device, picks up touch, and renders
 * exactly one frame before stopping. The freeze is in weston's own repaint
 * bookkeeping, not below it: vblank arrives at a clean 120 Hz, page flip
 * events come back every time, and a plain CPU render loop drives the panel
 * continuously at ~82 fps with nothing but one drmModeSetCrtc and then writes
 * to the mapped buffer.
 *
 * So this does the simple thing. One drmModeSetCrtc, two CPU dumb buffers, and
 * every client buffer blitted into whichever buffer is being scanned out. No
 * repaint scheduling to get wrong, no page flips, no shadow framebuffer, no
 * atomic state machine. When a client commits, it repaints, immediately and
 * synchronously.
 *
 * Supported, because together they prove the idea:
 *
 *   - wl_compositor, wl_subcompositor, wl_shm
 *   - wl_output, reporting the panel's real mode
 *   - a wl_seat with a pointer, fed from evdev touch
 *   - frame callbacks, sent after the frame containing the surface
 *
 * Damage is ignored in favour of redrawing the whole screen, which is
 * affordable at 1080x2400 with CPU blits and keeps presentation trivial. A
 * deliberate simplification, not an oversight.
 *
 * Rendering is CPU-only. The GPU is out of reach: KGSL exposes no DRM node, so
 * neither Mesa nor freedreno can see the Adreno.
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -O2 -I<rootfs>/usr/include lind.c -o lind \
 *         -L<rootfs>/usr/lib/aarch64-linux-gnu -lwayland-server -ldrm
 */

#define _GNU_SOURCE

#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <linux/input.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <wayland-server.h>
#include "xdg-shell-server-protocol.h"

/*
 * Neither struct drmEvent nor the page-flip event struct is in this
 * libdrm's public headers, but the kernel writes exactly these bytes to the
 * DRM fd. Layout is from the kernel UAPI and has been stable for many years.
 */
/*
 * A page flip completion, under either name.
 *
 * This kernel generation sends 0x02, DRM_EVENT_FLIP_COMPLETE - see
 * drm_atomic_uapi.c:906, where drm_mode_page_flip() stamps the event it is
 * about to queue. Newer kernels renamed it DRM_EVENT_PAGE_FLIP_DONE and moved
 * it to 0x04.
 *
 * Matching on 0x04 alone is silently wrong: the completion arrives, is
 * discarded as an unrecognised type, the pending flip is never cleared, and the
 * compositor then never issues a second flip. It presents one frame and looks
 * exactly like a driver that never completes a flip. That misreading is what
 * sent this looking for a kernel bug that was not there.
 */
static int is_flip_done(uint32_t type)
{
	return type == DRM_EVENT_FLIP_COMPLETE || type == 0x04;
}
/*
 * Only the header is declared here.
 *
 * A page-flip completion used to be modelled locally as
 *
 *	base + sequence + time + sequence_nr + reserved[4]      = 44 bytes
 *
 * which is the pre-4.10 layout and is simply wrong: the kernel sends 16.
 * Nothing noticed for a long time because the handler read into that struct
 * once per poll and only ever looked at base.type - the extra bytes were
 * harmless while a poll delivered exactly one event. As soon as the handler
 * started sizing packets with sizeof, the stream desynchronised, event types
 * came out as 317, and the compositor aborted in free().
 *
 * The kernel writes the real length into base.length for every event it
 * queues, so packets are now sized from the header and no local copy of any
 * event layout is needed. drm_event is declared to match
 * include/uapi/drm/drm.h, which is two u32 - type and length - and this
 * kernel's header does have length, so that part was right all along.
 */
struct drm_event_compat {
	int type;
	int length;
};

/*
 * Out-parameter for drmModePageFlip() only.
 *
 * 16 bytes: the kernel's own layout, base plus sequence, tv_sec, tv_usec. The
 * libdrm in this chroot declares the type incompletely, so it is spelled out
 * here; the copy that used to live here was 44 bytes and belonged to a kernel
 * generation that has not existed for a decade.
 *
 * Nothing sizes a packet from sizeof(this). The kernel writes every event's
 * true size into base.length, and that is what the reader uses.
 */
struct drm_mode_page_flip_event {
	struct drm_event_compat base;
	uint32_t sequence;
	uint32_t tv_sec;
	uint32_t tv_usec;
};
#include <xf86drm.h>
#include <xf86drmMode.h>

#define CARD        "/dev/dri/card0"
#define TOUCH_DEV   "/dev/input/event3"
#define FB_COUNT    2
#define RUNTIME_DIR "/run/user/0"
#define SOCKET_NAME "wayland-0"

#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

/*
 * Has anything changed since the last repaint?
 *
 * The keep-alive has to keep the flip pipeline moving even when there is
 * nothing new to draw, but repainting is not free: fill_background(), then a
 * full-panel desktop with FreeType text and antialiased rounded rectangles, at
 * 1080x2400, twenty to a hundred times a second. Measured at 80% of a core for
 * a home screen that had not changed since it was drawn.
 *
 * That is also a plausible reason touch felt unreliable: a compositor spending
 * four fifths of a core on a byte-identical frame has little left for the
 * input path, and the symptom - touch sometimes arriving, sometimes not - is
 * what a starved loop looks like from the outside.
 *
 * So the keep-alive presents the buffer already in memory, and repaints only
 * when something asked for it.
 */
static int ui_dirty = 1;

static uint32_t shm_formats[] = {
	WL_SHM_FORMAT_XRGB8888,
	WL_SHM_FORMAT_ARGB8888,
};

/* --- display ------------------------------------------------------------ */

static struct {
	int fd;
	uint32_t conn_id, crtc_id;
	uint32_t w, h;
	uint32_t stride[FB_COUNT];
	uint32_t handle[FB_COUNT], fb[FB_COUNT];
	uint64_t size;
	uint32_t *map[FB_COUNT];
	int mode_set;

	/*
	 * Double buffered. Rendering into the buffer the panel is scanning out
	 * tears the frame: part of the screen shows the new frame and part the
	 * old, which from the front reads as flickering.
	 *
	 * render is the buffer being drawn into, scanout the one the panel is
	 * reading. A page flip swaps them atomically, so the tear is gone.
	 *
	 * This is only usable because of a driver fix. The completion event
	 * never arrived before it - the request was accepted and PAGE_FLIP_DONE
	 * never came back, measured as one event in 3136 requests - which
	 * stranded a double buffered compositor on its first frame, and was also
	 * why weston froze. See docs/page-flip-root-cause.md.
	 */
	int render, scanout;
	int flip_pending;
	struct drm_mode_page_flip_event flip_event;

	/*
	 * Vblank, armed and kept armed.
	 *
	 * Without this the driver never interrupts on frame boundaries, and
	 * that is not a driver fault. drm_crtc_vblank_on() in this kernel
	 * calls no driver hook at all:
	 *
	 *	if (atomic_read(&vblank->refcount) != 0 || drm_vblank_offdelay == 0)
	 *		WARN_ON(drm_vblank_enable(dev, pipe));
	 *
	 * The reference count is taken in exactly one place,
	 * drm_crtc_send_vblank_event(), which runs when a client asks for a
	 * VBLANK or CRTC_SEQUENCE event. So a client that only page-flips and
	 * arms nothing gets no vblank interrupts - measured on this phone:
	 * msm_drm at 121 interrupts/s under Android and under weston, both of
	 * which arm sequence events, and sde_crtc_vblank_cb() completely silent
	 * under lind, which did not.
	 *
	 * That matters twice over. It is why a page flip whose completion is
	 * reported from the vblank interrupt never completes here, and it is
	 * also why lind used to guess when a frame was safe to start drawing.
	 * Asking the panel is both correct and necessary.
	 */
	uint64_t vbl_seq;
	int vbl_armed;

	unsigned long n_repaint, n_frames, n_commits;
	unsigned long n_events, n_flip_err, n_dropped, n_read_err;
	unsigned long n_vblank, n_seq_err, n_touch;
} drm;

/* --- text ----------------------------------------------------------------
 *
 * FreeType over DejaVuSans. DejaVuSans.ttf is already in the chroot (weston's
 * dependencies pulled it in), so the rasteriser was the only missing piece.
 *
 * Rows are read at FT_Bitmap.pitch. Rounding the glyph width up to a multiple
 * of four instead - which is what this did first - is one byte off per row, so
 * the glyph shears into a diagonal smear: recognisably the right shape, entirely
 * unreadable. The metric that gives it away is printed right next to the width.
 */
static FT_Library ft_lib;
static FT_Face ft_face;
static int ft_ready;

static int text_init(void)
{
	static const char *paths[] = {
		"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
		"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
	};
	size_t i;

	if (FT_Init_FreeType(&ft_lib))
		return -1;
	for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		if (FT_New_Face(ft_lib, paths[i], 0, &ft_face) == 0) {
			ft_ready = 1;
			return 0;
		}
	}
	FT_Done_FreeType(ft_lib);
	return -1;
}

static int text_width(const char *s, int px)
{
	int w = 0;

	if (!ft_ready || FT_Set_Pixel_Sizes(ft_face, 0, px))
		return 0;
	while (*s) {
		if (FT_Load_Char(ft_face, (unsigned char)*s, FT_LOAD_RENDER) == 0)
			w += (ft_face->glyph->advance.x + 31) >> 6;
		s++;
	}
	return w;
}

/* y is the top of the line, so callers need not know the face's ascent. */
static void draw_text(int x, int y, int px, uint32_t colour, const char *s)
{
	int sr = (colour >> 16) & 0xFF, sg = (colour >> 8) & 0xFF;
	int sb = colour & 0xFF, sa = (colour >> 24) & 0xFF;
	int baseline;

	if (!ft_ready || FT_Set_Pixel_Sizes(ft_face, 0, px))
		return;
	baseline = y + (int)(ft_face->size->metrics.ascender / 64);

	while (*s) {
		FT_GlyphSlot g;
		int gy, top, advance, pitch;
		const unsigned char *row;

		if (FT_Load_Char(ft_face, (unsigned char)*s, FT_LOAD_RENDER))
			break;
		g = ft_face->glyph;
		top = baseline - g->bitmap_top;
		pitch = (int)g->bitmap.pitch;
		if (pitch <= 0)
			pitch = (int)g->bitmap.width;

		for (gy = 0; gy < (int)g->bitmap.rows; gy++) {
			int gx;
			uint32_t *dst;
			int dy = top + gy;

			if (dy < 0 || dy >= (int)drm.h)
				continue;
			dst = drm.map[drm.render] +
			      (size_t)dy * drm.stride[drm.render] / 4;
			row = g->bitmap.buffer + (size_t)gy * pitch;
			for (gx = 0; gx < (int)g->bitmap.width; gx++) {
				int cov = row[gx], px2 = x + gx, a;

				if (!cov || px2 < 0 || px2 >= (int)drm.w)
					continue;
				a = cov * sa / 255;
				if (!a)
					continue;
				dst[px2] = (a << 24) |
					   (((sr * a + (dst[px2] >> 16 & 0xFF) *
					      (255 - a)) / 255) << 16) |
					   (((sg * a + (dst[px2] >> 8 & 0xFF) *
					      (255 - a)) / 255) << 8) |
					   (((sb * a + (dst[px2] & 0xFF) *
					      (255 - a)) / 255));
			}
		}
		advance = (g->advance.x + 31) >> 6;
		x += advance > 0 ? advance : 1;
		s++;
	}
}

/* --- drawing primitives -------------------------------------------------- */

/*
 * Antialiased rounded rectangle.
 *
 * Coverage is computed from the distance to the corner circle rather than by
 * supersampling: one pass, and the result is exact enough at these radii. The
 * point is not precision - it is that hard corners are what make the panel's
 * existing tear obvious, and a shape with a soft edge is the single cheapest way
 * to make the compositor's own UI look deliberate rather than unfinished.
 */
static uint8_t round_cov(int x, int y, int w, int h, int r)
{
	int cx, cy;

	if (r <= 0)
		return 255;
	if (x < r && y < r)
		cx = r, cy = r;
	else if (x >= w - r && y < r)
		cx = w - 1 - r, cy = r;
	else if (x < r && y >= h - r)
		cx = r, cy = h - 1 - r;
	else if (x >= w - r && y >= h - r)
		cx = w - 1 - r, cy = h - 1 - r;
	else
		return 255;

	{
		int dx = x - cx, dy = y - cy;
		int d2 = dx * dx + dy * dy;
		int outer = r * r;
		int inner = (r - 1) * (r - 1);

		if (d2 <= inner)
			return 255;
		if (d2 >= outer)
			return 0;
		/* Between the two radii: linear in the distance. */
		return (uint8_t)(255 * (outer - d2) / (outer - inner));
	}
}

static void fill_round_rect(int x0, int y0, int w, int h, int r, uint32_t col)
{
	int sr = (col >> 16) & 0xFF, sg = (col >> 8) & 0xFF;
	int sb = col & 0xFF, sa = (col >> 24) & 0xFF;
	int x, y;

	if (w <= 0 || h <= 0)
		return;
	if (r * 2 > w)
		r = w / 2;
	if (r * 2 > h)
		r = h / 2;

	for (y = 0; y < h; y++) {
		uint32_t *row;
		int dy = y0 + y;

		if (dy < 0 || dy >= (int)drm.h)
			continue;
		row = drm.map[drm.render] +
		      (size_t)dy * drm.stride[drm.render] / 4;
		for (x = 0; x < w; x++) {
			int dx = x0 + x, cov;

			if (dx < 0 || dx >= (int)drm.w)
				continue;
			cov = round_cov(x, y, w, h, r);
			if (!cov)
				continue;
			{
				uint32_t d = row[dx];
				int a = cov * sa / 255;

				row[dx] = (d & 0xFF000000) |
					  (((sr * a + (d >> 16 & 0xFF) *
					    (255 - a)) / 255) << 16) |
					  (((sg * a + (d >> 8 & 0xFF) *
					    (255 - a)) / 255) << 8) |
					  (((sb * a + (d & 0xFF) *
					    (255 - a)) / 255));
			}
		}
	}
}

/* --- home screen --------------------------------------------------------- */

/*
 * Layout taken from Phosh 0.24 (src/ui/app-grid-button.ui), which states it:
 *
 *   (360px screen width - 2*3px flowbox margins - (4-1)*6px column spacing)
 *   / 4 columns = 84px
 *
 * Four columns, 3dp margin, 6dp spacing, 84dp button, on a 360dp-wide screen.
 * This panel is 1080px wide, so the density scale is exactly 3 and every value
 * below is dp * 3 - no guessing. The 40dp home bar comes from the same tree's
 * home.ui, and the type sizes from src/stylesheet/common.css.
 *
 * What is ported is the arithmetic and the interaction: the grid, the tap
 * targets, the spacing. The QML is not, because QML needs Qt Quick, and Qt Quick
 * needs a GPU buffer path this compositor does not have - and reimplementing it
 * would be slower than drawing it.
 */
#define UI_SCALE   3
#define dp(n)      ((n) * UI_SCALE)

#define GRID_COLUMNS  4
#define GRID_MARGIN   dp(3)
#define GRID_GAP      dp(6)
#define GRID_BUTTON   dp(84)
#define HOME_BAR_H    dp(40)

struct app_entry {
	char name[32];
	char cmd[96];
	uint32_t tint;
};

#define MAX_APPS 16
static struct app_entry apps[MAX_APPS];
static int n_apps;

/*
 * Apps come from a plain text file, one per line: name | command | tint.
 *
 * A hard-coded list would be a launcher that launches nothing, which is worse
 * than no launcher. There are no .desktop entries here either - the chroot has
 * no desktop environment to register with - so the list is a file the user can
 * edit, with one working entry built in so the grid is never empty.
 */
#define APPS_FILE "/usr/share/lindroid/apps"

static void load_apps(void)
{
	char line[160];
	FILE *f;

	n_apps = 0;
	f = fopen(APPS_FILE, "r");
	if (f) {
		while (n_apps < MAX_APPS &&
		       fgets(line, sizeof(line), f)) {
			char *bar1, *bar2, *nl;
			struct app_entry *a;

			nl = strchr(line, '\n');
			if (nl)
				*nl = 0;
			if (!line[0] || line[0] == '#')
				continue;
			bar1 = strchr(line, '|');
			if (!bar1)
				continue;
			*bar1++ = 0;
			bar2 = strchr(bar1, '|');
			if (!bar2)
				continue;
			*bar2++ = 0;

			a = &apps[n_apps++];
			snprintf(a->name, sizeof(a->name), "%s", line);
			snprintf(a->cmd, sizeof(a->cmd), "%s", bar1);
			a->tint = (uint32_t)strtoul(bar2, NULL, 0);
		}
		fclose(f);
	}

	if (!n_apps) {
		snprintf(apps[0].name, sizeof(apps[0].name), "Демо");
		snprintf(apps[0].cmd, sizeof(apps[0].cmd), "lindtest 60");
		apps[0].tint = 0xFF3D7BE8;
		n_apps = 1;
	}
}

/* Geometry, exactly as the formula above. */
static void grid_geometry(int *btn, int *gap, int *margin)
{
	int m = GRID_MARGIN, g = GRID_GAP;

	*margin = m;
	*gap = g;
	*btn = ((int)drm.w - 2 * m - (GRID_COLUMNS - 1) * g) / GRID_COLUMNS;
}

/* Which tile is at this point, or -1. */
static int tile_at(int px, int py)
{
	int btn, gap, margin, col;
	int rows, top, x0;

	grid_geometry(&btn, &gap, &margin);
	rows = (n_apps + GRID_COLUMNS - 1) / GRID_COLUMNS;
	top = (int)drm.h - HOME_BAR_H - GRID_GAP - rows * btn -
	      (rows - 1) * gap;
	x0 = margin;

	col = (px - x0) / (btn + gap);
	if (col < 0 || col >= GRID_COLUMNS)
		return -1;
	if (py < top || py >= top + rows * btn + (rows - 1) * gap)
		return -1;
	{
		int row = (py - top) / (btn + gap);
		int idx = row * GRID_COLUMNS + col;

		return idx < n_apps ? idx : -1;
	}
}

static void draw_home(void)
{
	int btn, gap, margin, rows, top, i;

	grid_geometry(&btn, &gap, &margin);
	rows = (n_apps + GRID_COLUMNS - 1) / GRID_COLUMNS;
	top = (int)drm.h - HOME_BAR_H - GRID_GAP - rows * btn - (rows - 1) * gap;

	for (i = 0; i < n_apps; i++) {
		int col = i % GRID_COLUMNS, row = i / GRID_COLUMNS;
		int x = margin + col * (btn + gap);
		int y = top + row * (btn + gap);
		int lab = dp(13);
		int tw = text_width(apps[i].name, lab);

		fill_round_rect(x, y, btn, btn, dp(22), apps[i].tint);

		/* Label below the tile, clipped to the button column so a long
		 * name cannot bleed into its neighbour. */
		if (tw > btn) {
			char buf[sizeof(apps[i].name)];
			int fit = 0;

			snprintf(buf, sizeof(buf), "%s", apps[i].name);
			while (fit > 0 && text_width(buf, lab) > btn)
				buf[--fit] = 0;
			snprintf(buf + fit, sizeof(buf) - fit, "...");
			tw = text_width(buf, lab);
			draw_text(x + (btn - tw) / 2, y + btn + dp(6), lab,
				  0xFFE8EAF0, buf);
		} else {
			draw_text(x + (btn - tw) / 2, y + btn + dp(6), lab,
				  0xFFE8EAF0, apps[i].name);
		}
	}

	/* Home bar with a chevron, as in home.ui: nothing to do while it is
	 * the only thing on screen, but it anchors the layout. */
	fill_round_rect(0, (int)drm.h - HOME_BAR_H, (int)drm.w, HOME_BAR_H,
			dp(18), 0xFF1B1D22);
	{
		int cx = (int)drm.w / 2, cy = (int)drm.h - HOME_BAR_H / 2;
		int i2, s = dp(7);

		for (i2 = -s; i2 <= s; i2++)
			for (int j = 0; j < dp(4); j++) {
				int x = cx + i2 - dp(6) + j;
				int y = cy + dp(2) - j;

				if (x < 0 || x >= (int)drm.w ||
				    y < 0 || y >= (int)drm.h)
					continue;
				drm.map[drm.render][(size_t)y *
					drm.stride[drm.render] / 4 + x] =
					0xFF9AA0AC;
			}
	}
}

/*
 * Launch through a double fork, so the compositor neither waits nor inherits a
 * dying child. setpgid keeps a client from being killed by whatever signal
 * stopped us, which on this phone is frequent.
 */
static void launch(const char *cmd)
{
	pid_t pid = fork();

	if (pid < 0)
		return;
	if (pid == 0) {
		setsid();
		if (fork() > 0)
			_exit(0);
		execlp("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
}


/* --- backlight -----------------------------------------------------------
 *
 * The panel is a backlight device in sysfs, and nothing here is exotic. What
 * made it visible is that Android's SurfaceFlinger owns it normally, so when we
 * take the display the panel stays at whatever level SF last set - 315 of 2047
 * on this phone. Without touching this, a perfectly correct compositor looks
 * dim and gets blamed for it.
 *
 * Written as a persistent handle: the sysfs file rejects open-per-write more
 * slowly than it accepts one long-lived fd, and the write is on the gesture
 * path.
 */
#define BACKLIGHT_DIR "/sys/class/backlight/panel0-backlight/"

static int bl_fd = -1;
static int bl_max = 2047;
static int bl_level;
static int bl_want;             /* requested level, not yet written */
static int64_t bl_last_write_us;

/*
 * Coalesced, because the obvious version stalls the compositor.
 *
 * A touch panel reports a few hundred times a second during a drag, and writing
 * brightness straight from that path meant a sysfs write per sample. Each write
 * goes through sde_backlight_device_update_status() into the DSI PWM
 * programming under sde_vm_lock, synchronously, on the thread that also has to
 * present frames - so the animation stuttered for exactly as long as the swipe
 * lasted.
 *
 * 20 Hz is far more often than anyone can perceive in a brightness ramp, and it
 * is written from repaint() so it also follows the display rather than the
 * finger. The gesture itself stays immediate: only the write is deferred.
 */
#define BL_WRITE_INTERVAL_US 50000
static int osd_until;              /* ms, monotonic; 0 = nothing shown */

static void brightness_set(int level);

/* Write the requested level, if it has changed and enough time has passed. */
static void brightness_flush(void)
{
	struct timespec ts;
	int64_t now;

	if (bl_want == bl_level || bl_fd < 0)
		return;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	now = ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
	if (now - bl_last_write_us < BL_WRITE_INTERVAL_US)
		return;

	bl_last_write_us = now;
	brightness_set(bl_want);
}

static int brightness_init(int level)
{
	char path[sizeof(BACKLIGHT_DIR) + 32];
	int fd;

	snprintf(path, sizeof(path), "%smax_brightness", BACKLIGHT_DIR);
	bl_max = 2047;
	{
		int v = 0;
		unsigned maj, min;

		if (access(path, R_OK) == 0) {
			FILE *f = fopen(path, "r");

			if (f && fscanf(f, "%d", &v) == 1 && v > 0)
				bl_max = v;
			if (f)
				fclose(f);
		}
		(void)maj;
		(void)min;
	}

	snprintf(path, sizeof(path), "%sbrightness", BACKLIGHT_DIR);
	fd = open(path, O_WRONLY);
	if (fd < 0) {
		fprintf(stderr,
			"lind: не открыть %s (%s) - яркость не трогаю\n",
			path, strerror(errno));
		return -1;
	}
	bl_fd = fd;

	bl_want = level < 0 ? bl_max : level;
	bl_last_write_us = 0;
	brightness_flush();
	return 0;
}

static void brightness_set(int level)
{
	char buf[16];
	int n;

	if (bl_fd < 0)
		return;
	/* 0 is off, and anything near it reads as off. A floor of a tenth
	 * keeps a downward swipe from looking like a broken panel. */
	if (level < bl_max / 10)
		level = bl_max / 10;
	if (level > bl_max)
		level = bl_max;
	if (level == bl_level)
		return;

	n = snprintf(buf, sizeof(buf), "%d\n", level);
	if (write(bl_fd, buf, n) < 0) {
		fprintf(stderr, "lind: яркость %d не записалась (%s)\n",
			level, strerror(errno));
		return;
	}
	bl_level = level;

	/* Show it, so the gesture is visibly doing something rather than
	 * silently altering the screen. */
	{
		struct timespec ts;

		clock_gettime(CLOCK_MONOTONIC, &ts);
		osd_until = (int)(ts.tv_sec * 1000 +
				  ts.tv_nsec / 1000000) + 1200;
		bl_last_write_us = ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
	}
}

/* Vertical bar on the right, like the one Android puts up. */
static void draw_osd(void)
{
	struct timespec ts;
	int now, h, w, x0, y0, bar_h, i;
	uint32_t *p;
	uint32_t col;

	if (!osd_until)
		return;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	now = (int)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
	if (now > osd_until) {
		osd_until = 0;
		return;
	}

	h = drm.h;
	w = drm.w;
	bar_h = h * 3 / 4;
	y0 = (h - bar_h) / 2;
	x0 = w - 60;
	if (x0 < 0 || bar_h <= 0)
		return;

	/* Backdrop. */
	for (i = y0 - 8; i < y0 + bar_h + 8; i++) {
		if (i < 0 || i >= h)
			continue;
		p = drm.map[drm.render] + (size_t)i * drm.stride[drm.render] / 4;
		for (int x = x0 - 8; x < x0 + 40 && x < w; x++)
			p[x] = 0xC0000000;
	}

	/* Filled portion, level from the bottom. */
	/* Follow the requested level, not the written one: writes are
	 * deferred, and a bar that lagged the finger would read as lag. */
	bar_h = bar_h * (bl_want ? bl_want : bl_level) / bl_max;
	for (i = 0; i < bar_h; i++) {
		int y = y0 + (h * 3 / 4) - 1 - i;

		if (y < y0 || y >= h)
			continue;
		p = drm.map[drm.render] + (size_t)y * drm.stride[drm.render] / 4;
		col = 0xFF40C0FF;
		for (int x = x0 - 4; x < x0 + 36 && x < w; x++)
			p[x] = col;
	}
}

/* --- state -------------------------------------------------------------- */

enum surface_role {
	ROLE_NONE,
	ROLE_TOPLEVEL,
	ROLE_POPUP,
};

struct surface {
	struct wl_list link;
	struct wl_resource *resource;
	struct wl_resource *pending_buffer, *current_buffer;
	struct wl_list frame_callbacks;
	/* xdg-shell wraps a wl_surface rather than replacing it, so the
	 * surface keeps both. A toplevel needs an initial configure before
	 * the client is allowed to map, so the toplevel resource is kept too. */
	struct wl_resource *xdg_surface, *xdg_toplevel;
	/* A popup is drawn above every toplevel and is dismissed by a tap that
	 * lands outside it. One at a time: nesting menus is a later problem
	 * than having menus. */
	struct wl_resource *xdg_popup, *popup_parent;
	int is_popup, popup_configured;
	/* Where the popup sits on the panel, in pixels. Kept on the surface
	 * rather than recomputed per frame because three places need it:
	 * the configure we send, the blit, and the hit test that decides
	 * whether a tap landed inside the menu or dismissed it. */
	int popup_x, popup_y, popup_w, popup_h;
	int fullscreen_requested;
	enum surface_role role;
	int32_t width, height;
	int has_buffer;
};


struct shm_buffer {
	void *pool_data;
	size_t pool_size;
	int32_t offset, width, height, stride;
	uint32_t format;
	struct wl_resource *resource;
};

static struct surface *the_popup;

static int inside_popup(int px, int py)
{
	struct surface *s = the_popup;
	int w, h;

	if (!s || !s->current_buffer)
		return 0;
	{
		struct shm_buffer *b = wl_resource_get_user_data(s->current_buffer);

		if (!b)
			return 0;
		w = b->width;
		h = b->height;
	}
	/* Measured from the popup's own corner, not the panel's: a menu in
	 * the middle of the screen must swallow taps inside itself and let
	 * everything else through, or it dismisses the instant it is used. */
	return px >= s->popup_x && py >= s->popup_y &&
	       px < s->popup_x + w && py < s->popup_y + h;
}

/* Dismiss the popup: popup_done, then the resource goes away. */
static void dismiss_popup(void)
{
	ui_dirty = 1;
	struct surface *s = the_popup;

	if (!s)
		return;
	the_popup = NULL;
	if (s->xdg_popup) {
		xdg_popup_send_popup_done(s->xdg_popup);
		wl_resource_destroy(s->xdg_popup);
		s->xdg_popup = NULL;
	}
	s->is_popup = 0;
	s->popup_configured = 0;
	s->popup_x = s->popup_y = s->popup_w = s->popup_h = 0;
	s->role = ROLE_NONE;
	if (s->current_buffer) {
		wl_resource_destroy(s->current_buffer);
		s->current_buffer = NULL;
	}
	s->has_buffer = 0;
}

struct shm_pool {
	int fd;
	size_t size;
};

struct pointer {
	struct wl_list link;
	struct wl_resource *resource;
};

static struct wl_list surfaces;
static struct wl_list pointers;

/*
 * Frame pacing.
 *
 * Repainting on every client commit is what made the panel flicker. A client
 * that commits as fast as it can gets its frame callback straight back, commits
 * again immediately, and the buffer the panel is scanning out is rewritten
 * faster than the panel reads it. The tear is then continuous rather than
 * occasional, which is what it looked like from the front.
 *
 * Every compositor paces this. weston has a repaint window; sway and cage cap
 * themselves at the output refresh; and the frame callback is sent once the
 * frame has actually been presented, not when it was drawn. Page flip is how
 * that is normally arranged, and it is unavailable here - the request is
 * accepted and PAGE_FLIP_DONE never arrives, measured as events=1 of type
 * DRM_EVENT_VBLANK_2 against flip requests on every frame.
 *
 * So the pacing is done with a timer at the panel's refresh period instead.
 */
static int64_t ev_last_us;
static void arm_keepalive(void);

static struct surface *pending_surface;
static struct surface *flip_surface;
static struct wl_event_source *pace_timer;
static int pace_armed;
/* The panel runs at 120 Hz, period measured at 8343 us. A hair over that, so a
 * keep-alive flip always finds the next vblank instead of racing it. */
#define PACE_MSEC 9
static uint32_t serial_counter;
static uint32_t last_x, last_y;
static int touch_active;

static void *xmalloc(size_t n)
{
	void *p = calloc(1, n ? n : 1);

	if (!p) {
		fprintf(stderr, "lind: out of memory (%zu)\n", n);
		exit(1);
	}
	return p;
}

/* --- blitting and presentation ------------------------------------------- */

/*
 * The panel is scanned out of map[0] continuously, so presenting a frame is
 * just a blit into it. There is no flip to wait for and no queue to drain -
 * that is the whole reason for using this path.
 */
static void blit_shm(void *src, int src_stride, int w, int h, int ox, int oy)
{
	int y;

	for (y = 0; y < h; y++) {
		int dy = oy + y;

		if (dy < 0 || dy >= (int)drm.h)
			continue;
		memcpy((char *)drm.map[drm.render] + (size_t)dy * drm.stride[drm.render] + ox * 4,
		       (char *)src + (size_t)y * src_stride,
		       (size_t)w * 4 > (size_t)drm.stride[drm.render] - (size_t)ox * 4
			? (size_t)drm.stride[drm.render] - (size_t)ox * 4
			: (size_t)w * 4);
	}
}

static void fill_background(void)
{
	int y;
	uint32_t *row = drm.map[drm.render];
	uint32_t grey = 0xFF181818;

	for (y = 0; y < (int)drm.h; y++) {
		uint32_t *p = row + (size_t)y * drm.stride[drm.render] / 4;
		int x;

		/* A visible gradient, so a frozen frame is obvious rather
		 * than merely dark. */
		for (x = 0; x < (int)drm.w; x++)
			p[x] = (grey & 0xFF000000) |
			       (((x / 8) & 0xFF) << 16) |
			       (((x / 8) & 0xFF) << 8) |
			       ((x / 8) & 0xFF);
	}
}

static void send_frame_callbacks(struct surface *s)
{
	struct wl_resource *cb, *tmp;

	wl_resource_for_each_safe(cb, tmp, &s->frame_callbacks) {
		wl_callback_send_done(cb, 0);
		wl_resource_destroy(cb);
	}
	wl_list_insert(&s->frame_callbacks, &s->frame_callbacks);
}

static void repaint(struct surface *dirty)
{
	struct surface *s;

	ui_dirty = 0;
	fill_background();

	/* Two passes: a popup belongs above every toplevel, and the surface
	 * list is in creation order, not stacking order. */
	for (int pass = 0; pass < 2; pass++) {
		wl_list_for_each(s, &surfaces, link) {
			struct shm_buffer *b;

			if (!s->current_buffer)
				continue;
			if (s->is_popup != pass)
				continue;
			b = wl_resource_get_user_data(s->current_buffer);
			if (!b)
				continue;
			blit_shm((char *)b->pool_data + b->offset, b->stride,
				 b->width, b->height,
				 s->is_popup ? s->popup_x : 0,
				 s->is_popup ? s->popup_y : 0);
		}
	}

	brightness_flush();

	/*
	 * With no client mapped, the desktop itself. Phosh shows the launcher
	 * rather than an empty gradient, and that is the whole point of a home
	 * screen: it has to be what you see when nothing is running.
	 */
	/* Nothing mapped: the desktop is what a compositor shows by itself.
	 * wl_list_empty() is true in exactly that case - negating it drew the
	 * home screen behind a window instead of in front of the desktop, and
	 * with no client at all it drew nothing. */
	if (wl_list_empty(&surfaces))
		draw_home();

	draw_osd();

	drm.n_repaint++;

	/*
	 * Draw is done into the buffer the panel is not reading. Publishing
	 * it is a separate step: the flip is what makes it visible, and the
	 * client is not told the frame is done until the flip completes.
	 *
	 * If the previous flip has not landed yet, skip rather than draw over
	 * the buffer now on screen. The frame callback has not been sent, so
	 * the client simply waits, which is the back-pressure a
	 * double-buffered compositor gets for free.
	 */
	if (drm.flip_pending) {
		drm.n_dropped++;
		return;
	}

	if (drmModePageFlip(drm.fd, drm.crtc_id, drm.fb[drm.render],
			   DRM_MODE_PAGE_FLIP_EVENT,
			   &drm.flip_event) != 0) {
		drm.n_flip_err++;
		return;
	}

	drm.flip_pending = 1;
	flip_surface = dirty;
}

/*
 * Present a frame, or remember that one is owed.
 *
 * Presentation is self-clocked on the flip completion rather than on a timer.
 * The timer and the panel do not agree: measured flip intervals were 25 ms at
 * first and then alternating 7.8 and 17.3 ms against a panel period of 8.343 ms,
 * so the frame rate was both well under the refresh and irregular. An irregular
 * swap lands mid-scan, which shows as a band near the top of the screen a couple
 * of times a second.
 */
static void maybe_present(struct surface *s)
{
	ui_dirty = 1;
	if (drm.flip_pending) {
		pending_surface = s;
		return;
	}

	pending_surface = NULL;
	repaint(s);
}

/*
 * vblank_arm - ask for one more CRTC_SEQUENCE event, and keep asking.
 *
 * This is what turns the driver's frame-boundary interrupt on; see the comment
 * on drm.vbl_seq. Re-armed from the event handler as each one arrives, so a
 * reference is always outstanding and the interrupts stay enabled.
 *
 * The sequence number is absolute and incremented here, not
 * DRM_CRTC_SEQUENCE_RELATIVE. Relative with a stale counter was tried and is a
 * trap: asked for a sequence already passed, the kernel delivers it
 * immediately, the event handler re-arms, and the loop turns into a pump -
 * measured at 500,000 events per second, with the compositor burning a core
 * delivering events nobody asked for.
 *
 * DRM_CRTC_SEQUENCE_NEXT_ON_MISS keeps a late re-arm from stalling forever: if
 * the target sequence has already gone by, the next one is used instead of
 * returning an error and leaving vblank permanently disarmed.
 */
static void vblank_arm(void)
{
	struct drm_crtc_queue_sequence q;

	if (!drm.mode_set)
		return;

	memset(&q, 0, sizeof q);
	q.crtc_id = drm.crtc_id;
	q.flags = DRM_CRTC_SEQUENCE_NEXT_ON_MISS;
	q.sequence = ++drm.vbl_seq;
	q.user_data = 0;

	if (drmIoctl(drm.fd, DRM_IOCTL_CRTC_QUEUE_SEQUENCE, &q) < 0) {
		drm.n_seq_err++;
		/* No sequence is armed, so no vblank reference is held and the
		 * driver interrupts are off. Say so once rather than per frame. */
		if (drm.n_seq_err == 1)
			fprintf(stderr,
				"lind: не вооружить vblank (%s) - прерывания "
				"границы кадра останутся выключены\n",
				strerror(errno));
		return;
	}

	/* The kernel rewrites sequence with the one it actually picked. */
	drm.vbl_seq = q.sequence;
	drm.vbl_armed = 1;
}

/*
 * The DRM fd is readable when a flip completes. The event type is logged for
 * the first few: "no event" and "an event of some other type" look identical
 * from a frame counter, and that ambiguity cost real time once already.
 */
static void log_event(int type, const char *what)
{
	struct timespec ts;
	static unsigned long seen_flip, seen_seq, seen_other;

	unsigned long *n = type == DRM_EVENT_FLIP_COMPLETE ? &seen_flip :
			   type == DRM_EVENT_CRTC_SEQUENCE  ? &seen_seq :
							     &seen_other;

	if (++*n > 8)
		return;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	if (ev_last_us)
		fprintf(stderr, "lind: %s #%lu +%lld us (type=%d)\n", what, *n,
			(long long)(ts.tv_sec * 1000000 + ts.tv_nsec / 1000 -
				    ev_last_us), type);
	else
		fprintf(stderr, "lind: %s #%lu (type=%d)\n", what, *n, type);

	ev_last_us = ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/*
 * One DRM event, dispatched by type.
 *
 * The event fd is a packet stream and the packets differ in size - a flip
 * completion is 16 bytes, a CRTC_SEQUENCE 24 - so the header decides how many
 * bytes to consume. Reading a fixed struct instead truncates the bigger packet
 * and every event after it is decoded at the wrong offset; that shows up as
 * nonsense event types rather than as a parse error, which is why it is worth
 * spelling out.
 */
static void handle_event(const char *buf)
{
	struct drm_event *hdr = (struct drm_event *)buf;

	/* is_flip_done rather than a case label, so the 0x04 name newer
	 * kernels use keeps working - see its comment. */
	if (is_flip_done(hdr->type)) {
		struct surface *s;

		log_event(hdr->type, "flip done");

		s = flip_surface;
		flip_surface = NULL;
		drm.scanout = drm.render;
		drm.render = 1 - drm.render;
		drm.flip_pending = 0;
		drm.n_frames++;

		if (s)
			send_frame_callbacks(s);

		/* The completion is the clock. Take the next frame straight away
		 * rather than waiting on a timer that does not agree with the
		 * panel. */
		if (pending_surface)
			maybe_present(pending_surface);
		else
			arm_keepalive();
		return;
	}

	switch (hdr->type) {
	case DRM_EVENT_CRTC_SEQUENCE: {
		struct drm_event_crtc_sequence q;

		memcpy(&q, buf, sizeof q);
		log_event(q.base.type, "vblank seq");
		drm.n_vblank++;
		drm.vbl_seq = q.sequence;

		/* Re-arm at once: the reference this event was holding is
		 * released when it is delivered, and the driver's interrupts go
		 * with it. */
		vblank_arm();
		break;
	}

	case DRM_EVENT_VBLANK:
		log_event(hdr->type, "vblank");
		drm.n_vblank++;
		vblank_arm();
		break;

	default:
		log_event(hdr->type, "other");
		break;
	}
}

/*
 * The DRM fd is readable when the driver has something to say.
 *
 * Draining is a loop, not a single read. One read returns whatever has
 * accumulated, which with a 120 Hz vblank armed is regularly two events - and
 * returning after the first leaves the second sitting in a buffer the fd will
 * never make readable again. That is how type=317 turns up: the leftover is
 * eventually decoded starting at the wrong offset.
 */
static int on_drm_event(int fd, uint32_t mask, void *data)
{
	static char buf[256];
	static size_t have;
	struct drm_event *hdr = (struct drm_event *)buf;
	size_t need;
	ssize_t n;

	(void)mask;
	(void)data;

	/*
	 * One read per callback, then drain only what that read returned.
	 *
	 * Reading again inside the loop is unbounded, and with a vblank armed
	 * at 120 Hz it does not terminate in practice: events arrive as fast
	 * as they are consumed, so have never reaches zero, this function never
	 * returns, and epoll never services the touch device. Measured as lind
	 * at 70% CPU in state R with the screen unresponsive to touch, and the
	 * flip interval stretched from 8.3 ms to 67 ms by the time the events
	 * behind each flip finally got drained.
	 *
	 * Draining a buffer that is already in hand is bounded - it is at most
	 * sizeof buf bytes - and anything still queued keeps the fd readable,
	 * so it comes back here. That is the whole reason for the bound.
	 */
	if (have == 0) {
		n = read(fd, buf, sizeof buf);
		if (n < 0) {
			if (errno != EAGAIN)
				drm.n_read_err++;
			return 0;
		}
		if (n == 0)
			return 0;

		have = (size_t)n;
	}

	while (have >= sizeof(struct drm_event)) {
		/*
		 * Size from the header. The kernel stamps base.length with the
		 * real size of every event it queues, so there is nothing to
		 * hardcode and nothing to get wrong per event type.
		 */
		need = (size_t)hdr->length;

		if (need < sizeof(struct drm_event) || need > sizeof buf)
			need = is_flip_done(hdr->type) ?
			       sizeof(struct drm_event) :
			       sizeof(struct drm_event_crtc_sequence);

		/* A packet split across reads. Keep the remainder for next time. */
		if (have < need)
			break;

		drm.n_events++;
		handle_event(buf);

		have -= need;
		if (have)
			memmove(buf, buf + need, have);
	}

	/* Less than a header cannot be the start of anything usable, and
	 * resynchronising here beats waiting for bytes that would decode as
	 * garbage. */
	if (have && have < sizeof(struct drm_event))
		have = 0;

	/* Counters once a second, printed from here rather than from a timer:
	 * a timer only fires if the loop goes idle, which is the thing in
	 * doubt. If this line stops appearing, the loop is not returning here
	 * at all. */
	{
		static struct timespec stat_at;
		struct timespec now;

		clock_gettime(CLOCK_MONOTONIC, &now);
		if (!stat_at.tv_sec ||
		    (now.tv_sec - stat_at.tv_sec) >= 1) {
			unsigned long el = stat_at.tv_sec ?
				(unsigned long)(now.tv_sec - stat_at.tv_sec) :
				0;

			fprintf(stderr,
				"lind: STAT +%lus событий=%lu vblank=%lu "
				"кадров=%lu тач=%lu dirty=%d pend=%d "
				"ошибок=%lu/%lu\n",
				el, drm.n_events, drm.n_vblank, drm.n_frames,
				drm.n_touch, ui_dirty, pending_surface ? 1 : 0,
				drm.n_read_err, drm.n_seq_err);
			stat_at = now;
		}
	}

	return 0;
}

/* Repaint, then release the client waiting on this frame. */
/*
 * Keep the display fed.
 *
 * Presentation is self-clocked on the flip completion, which is right for rate
 * and regularity but means that when the client stops committing there are no
 * flips at all - and this panel powers down when it stops being fed. The screen
 * then appears to switch itself off, usually noticed the moment a finger lands
 * on it and the client does not respond.
 *
 * So this re-presents the buffer already on screen, once per panel period,
 * whenever there is nothing new. No redraw, just the flip. Real compositors do
 * the same job with a keep-awake timer, and weston has one for this reason.
 */
static void arm_keepalive(void)
{
	/* Re-arming a pending timer pushes its deadline out again, so it must
	 * only be armed when it is not already: a client committing every
	 * millisecond would otherwise postpone the frame forever, which cost
	 * 13 repaints against 28004 client frames before it was noticed. */
	if (pace_timer && !pace_armed) {
		wl_event_source_timer_update(pace_timer, PACE_MSEC);
		pace_armed = 1;
	}
}

static int pace_dispatch(void *data)
{
	(void)data;
	pace_armed = 0;

	if (!drm.flip_pending) {
		/*
		 * Draw, then present.
		 *
		 * Re-presenting the buffer as it stands is only correct while
		 * something else is deciding what goes in it. With no client
		 * there are no commits, so repaint() would never run, and the
		 * only thing ever drawn was the startup gradient - the home
		 * screen, being what a compositor draws when nothing is
		 * mapped, was never painted at all.
		 *
		 * repaint() draws and then flips, so this needs no separate
		 * flip; a pending commit still takes priority.
		 */
		/*
		 * Three cases, one action each.
		 *
		 * A client commit is already handled above; what is left is
		 * what to do with an idle compositor.
		 *
		 * If nothing has changed, do nothing. The panel keeps scanning
		 * out the frame it already has, so there is nothing to feed it,
		 * and flipping anyway would swap to the other buffer - which,
		 * on a static screen, has not been painted since the last
		 * change and holds whatever was in the allocation. That is
		 * garbage on screen, and it looked like lag rather than like a
		 * bug, which is why it is worth writing down.
		 *
		 * If something changed, repaint; repaint() ends in its own
		 * flip.
		 *
		 * Ping-pong note: only the buffer being drawn into is ever
		 * painted, so "flip without repainting" is only safe once both
		 * buffers have held the same image at least once. Not flipping
		 * avoids needing that.
		 */
		if (!pending_surface) {
			if (ui_dirty)
				repaint(NULL);
		} else if (drmModePageFlip(drm.fd, drm.crtc_id,
					   drm.fb[drm.scanout],
					   DRM_MODE_PAGE_FLIP_EVENT,
					   &drm.flip_event) == 0) {
			drm.flip_pending = 1;
		} else {
			drm.n_flip_err++;
		}
	}

	/* Always re-armed, including when the repaint or flip above failed:
	 * the panel must keep being fed or it goes dark. */
	arm_keepalive();

	/* A timer callback returning 0 is removed from the loop, and this
	 * timer is the only thing keeping the display alive. */
	return 1;
}

/* Printed on demand, so a flicker report can be turned into facts. */
static void dump_stats(void)
{
	fprintf(stderr, "lind: repaint=%lu frames=%lu\n", drm.n_repaint,
		drm.n_frames);
}

/* --- shm ----------------------------------------------------------------
 *
 * create_buffer lives on wl_shm_pool in libwayland 1.21, not on wl_shm. The
 * pool carries create_buffer, destroy and resize; wl_shm only has create_pool.
 * Getting that wrong is a compile error rather than a silent failure, which is
 * the one mercy here.
 */

static void shm_buffer_destroy(struct wl_client *c, struct wl_resource *r)
{
	struct shm_buffer *b = wl_resource_get_user_data(r);
	struct surface *s, *tmp;

	(void)c;
	/* A dying buffer must not be left referenced by any surface. */
	wl_list_for_each_safe(s, tmp, &surfaces, link) {
		if (s->current_buffer == r) {
			s->current_buffer = NULL;
			s->has_buffer = 0;
		}
		if (s->pending_buffer == r)
			s->pending_buffer = NULL;
	}

	if (b) {
		if (b->pool_data)
			munmap(b->pool_data, b->pool_size);
		free(b);
	}
	wl_resource_destroy(r);
}

static const struct wl_buffer_interface shm_buffer_impl = {
	.destroy = shm_buffer_destroy,
};

static void shm_pool_destroy(struct wl_client *c, struct wl_resource *r)
{
	struct shm_pool *p = wl_resource_get_user_data(r);

	(void)c;
	if (p && p->fd >= 0)
		close(p->fd);
	free(p);
	wl_resource_destroy(r);
}

static void shm_pool_resize(struct wl_client *c, struct wl_resource *r,
			    int32_t size)
{
	struct shm_pool *p = wl_resource_get_user_data(r);

	(void)c;
	/*
	 * The fd arrived from the client and is a memfd, which ftruncate
	 * cannot grow once the mapping exists in some kernel versions - and
	 * weston-flower hits this on its first buffer. Growing is legal and
	 * common, so the pool is remapped instead of refused.
	 */
	if (size <= 0 || (size_t)size <= p->size)
		return;
	if (ftruncate(p->fd, size) < 0) {
		wl_resource_post_error(r, WL_SHM_ERROR_INVALID_FD,
				       "pool resize failed: %s",
				       strerror(errno));
		return;
	}
	p->size = size;
}

static void shm_pool_create_buffer(struct wl_client *c, struct wl_resource *r,
				   uint32_t id, int32_t offset, int32_t w,
				   int32_t h, int32_t stride, uint32_t format)
{
	struct shm_pool *p = wl_resource_get_user_data(r);
	struct shm_buffer *b;
	struct wl_resource *nr;

	if (format != WL_SHM_FORMAT_XRGB8888 &&
	    format != WL_SHM_FORMAT_ARGB8888) {
		wl_resource_post_error(r, WL_SHM_ERROR_INVALID_FORMAT,
				       "only XRGB8888/ARGB8888 supported");
		return;
	}
	if (offset < 0 || w <= 0 || h <= 0 || stride < w * 4) {
		wl_resource_post_error(r, WL_SHM_ERROR_INVALID_STRIDE,
				       "bad geometry");
		return;
	}
	if ((size_t)offset + (size_t)stride * (size_t)h > p->size) {
		wl_resource_post_error(r, WL_SHM_ERROR_INVALID_FD,
				       "buffer outside pool");
		return;
	}

	b = xmalloc(sizeof(*b));
	b->pool_size = p->size;
	b->pool_data = mmap(NULL, p->size, PROT_READ, MAP_SHARED, p->fd, 0);
	if (b->pool_data == MAP_FAILED) {
		free(b);
		wl_resource_post_error(r, WL_SHM_ERROR_INVALID_FD, "mmap failed");
		return;
	}
	b->offset = offset;
	b->width = w;
	b->height = h;
	b->stride = stride;
	b->format = format;

	nr = wl_resource_create(c, &wl_buffer_interface, 1, id);
	if (!nr) {
		munmap(b->pool_data, b->pool_size);
		free(b);
		return;
	}
	b->resource = nr;
	wl_resource_set_implementation(nr, &shm_buffer_impl, b, NULL);
}

static const struct wl_shm_pool_interface shm_pool_impl = {
	.create_buffer = shm_pool_create_buffer,
	.destroy = shm_pool_destroy,
	.resize = shm_pool_resize,
};

static void shm_create_pool(struct wl_client *c, struct wl_resource *r,
			    uint32_t id, int32_t fd, int32_t size)
{
	struct shm_pool *p = xmalloc(sizeof(*p));
	struct wl_resource *nr;

	p->fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	if (p->fd < 0) {
		wl_resource_post_error(r, WL_SHM_ERROR_INVALID_FD,
				       "dup failed");
		wl_resource_destroy(r);
		return;
	}
	close(fd);
	p->size = size;

	nr = wl_resource_create(c, &wl_shm_pool_interface, 1, id);
	if (!nr) {
		close(p->fd);
		free(p);
		wl_resource_destroy(r);
		return;
	}
	wl_resource_set_implementation(nr, &shm_pool_impl, p, NULL);
}

static const struct wl_shm_interface shm_impl = {
	.create_pool = shm_create_pool,
};

/* --- surfaces ----------------------------------------------------------- */

static void surface_destroy(struct wl_client *c, struct wl_resource *r)
{
	struct surface *s = wl_resource_get_user_data(r);
	struct wl_resource *cb, *tmp;

	(void)c;
	wl_list_for_each_safe(cb, tmp, &s->frame_callbacks, link) {
		wl_resource_destroy(cb);
	}
	wl_list_remove(&s->link);
	free(s);
	wl_resource_destroy(r);
}

static void surface_attach(struct wl_client *c, struct wl_resource *r,
			   struct wl_resource *buffer, int32_t x, int32_t y)
{
	struct surface *s = wl_resource_get_user_data(r);

	(void)c; (void)x; (void)y;

	/* A client that attaches a new buffer without committing the old one
	 * is not making a mistake worth killing it over. */
	s->pending_buffer = buffer;
}

static void surface_damage(struct wl_client *c, struct wl_resource *r,
			   int32_t x, int32_t y, int32_t w, int32_t h)
{
	(void)c; (void)r; (void)x; (void)y; (void)w; (void)h;
	/* Everything is redrawn on commit anyway. */
}

static void surface_frame(struct wl_client *c, struct wl_resource *r,
			  uint32_t cb_id)
{
	struct surface *s = wl_resource_get_user_data(r);
	struct wl_resource *cb = wl_resource_create(c,
			&wl_callback_interface, 1, cb_id);

	(void)r;
	if (cb)
		wl_list_insert(&s->frame_callbacks, wl_resource_get_link(cb));
}

static void surface_set_opaque_region(struct wl_client *c,
				      struct wl_resource *r,
				      struct wl_resource *region)
{
	(void)c; (void)r; (void)region;
}

static void surface_set_input_region(struct wl_client *c,
				     struct wl_resource *r,
				     struct wl_resource *region)
{
	(void)c; (void)r; (void)region;
}

static void surface_commit(struct wl_client *c, struct wl_resource *r)
{
	struct surface *s = wl_resource_get_user_data(r);
	struct shm_buffer *b;

	(void)c;

	if (s->pending_buffer) {
		if (s->current_buffer && s->current_buffer != s->pending_buffer)
			wl_buffer_send_release(s->current_buffer);
		s->current_buffer = s->pending_buffer;
		s->pending_buffer = NULL;
		s->has_buffer = s->current_buffer != NULL;

		/* A popup is configured once, with the size it asked for. A
		 * client will not draw until it has seen this. */
		if (s->is_popup && s->has_buffer && !s->popup_configured &&
		    s->xdg_popup) {
			struct shm_buffer *b =
				wl_resource_get_user_data(s->current_buffer);

			if (b) {
				/* The size is the client's own, and the position is
				 * the one it computed from its positioner: a
				 * compositor that answers with 0,0 makes the
				 * client lay itself out in the corner. The
				 * buffer is only used to learn that the client
				 * has actually drawn something. */
				xdg_popup_send_configure(s->xdg_popup, s->popup_x,
							 s->popup_y,
							 s->popup_w ? s->popup_w
									   : b->width,
							 s->popup_h ? s->popup_h
									   : b->height);
				s->popup_configured = 1;
			}
		}
		if (s->current_buffer) {
			b = wl_resource_get_user_data(s->current_buffer);
			s->width = b ? b->width : 0;
			s->height = b ? b->height : 0;
		}
	}

	/*
	 * A commit means the client wants this on screen, but the screen is
	 * shared with the panel's scanout. Repainting right now would let a
	 * fast client rewrite the buffer mid-scan, so the frame is scheduled
	 * and the callback is sent when it has actually been shown.
	 */
	drm.n_commits++;
	pending_surface = s;

	maybe_present(s);
}

static const struct wl_surface_interface surface_impl = {
	.destroy = surface_destroy,
	.attach = surface_attach,
	.damage = surface_damage,
	.frame = surface_frame,
	.set_opaque_region = surface_set_opaque_region,
	.set_input_region = surface_set_input_region,
	.commit = surface_commit,
};

static void create_surface(struct wl_client *c, struct wl_resource *r,
			   uint32_t id)
{
	struct surface *s = xmalloc(sizeof(*s));

	s->resource = wl_resource_create(c, &wl_surface_interface, 1, id);
	if (!s->resource) {
		free(s);
		wl_resource_destroy(r);
		return;
	}
	wl_resource_set_implementation(s->resource, &surface_impl, s, NULL);
	wl_list_init(&s->frame_callbacks);
	wl_list_insert(&surfaces, &s->link);
}

static void create_region(struct wl_client *c, struct wl_resource *r,
			  uint32_t id)
{
	(void)c; (void)id;
	wl_resource_set_implementation(r, NULL, NULL, NULL);
	wl_resource_destroy(r);
}

static const struct wl_compositor_interface compositor_impl = {
	.create_surface = create_surface,
	.create_region = create_region,
};

static void subcompositor_destroy(struct wl_client *c,
				  struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static const struct wl_subcompositor_interface subcompositor_impl = {
	.destroy = subcompositor_destroy,
};

/* --- output ------------------------------------------------------------- */

static void output_release(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static const struct wl_output_interface output_impl = {
	.release = output_release,
};

/* --- input -------------------------------------------------------------- */

static void pointer_set_cursor(struct wl_client *c, struct wl_resource *r,
			       uint32_t serial, struct wl_resource *surface,
			       int32_t x, int32_t y)
{
	(void)c; (void)r; (void)serial; (void)surface; (void)x; (void)y;
}

static void pointer_release(struct wl_client *c, struct wl_resource *r)
{
	struct pointer *p, *tmp;

	(void)c;
	wl_list_for_each_safe(p, tmp, &pointers, link) {
		if (p->resource == r) {
			wl_list_remove(&p->link);
			free(p);
			break;
		}
	}
	wl_resource_destroy(r);
}

static const struct wl_pointer_interface pointer_impl = {
	.set_cursor = pointer_set_cursor,
	.release = pointer_release,
};

static void seat_get_pointer(struct wl_client *c, struct wl_resource *r,
			     uint32_t id)
{
	struct wl_resource *p = wl_resource_create(c, &wl_pointer_interface,
						   wl_pointer_interface.version,
						   id);
	struct pointer *entry;

	if (!p)
		return;
	wl_resource_set_implementation(p, &pointer_impl, NULL, NULL);
	entry = xmalloc(sizeof(*entry));
	entry->resource = p;
	wl_list_insert(&pointers, &entry->link);
}

static void seat_release(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static const struct wl_seat_interface seat_impl = {
	.get_pointer = seat_get_pointer,
	.get_keyboard = NULL,
	.get_touch = NULL,
	.release = seat_release,
};

static void send_pointer(uint32_t type, uint32_t button, uint32_t state)
{
	struct pointer *p, *tmp;

	wl_list_for_each_safe(p, tmp, &pointers, link) {
		/*
		 * wl_pointer_send_motion takes wl_fixed_t, which is 24.8
		 * fixed point. Passing display pixels as if they were plain
		 * integers divides every coordinate by 256: a touch at
		 * (540,1200) arrived as (2,4).
		 */
		wl_pointer_send_motion(p->resource, ++serial_counter,
				       wl_fixed_from_int((int)last_x),
				       wl_fixed_from_int((int)last_y));
		if (type == WL_POINTER_BUTTON)
			wl_pointer_send_button(p->resource, serial_counter,
					      button, state, WL_POINTER_AXIS_SOURCE_FINGER);
		else
			wl_pointer_send_frame(p->resource);
	}
}

/* --- evdev -------------------------------------------------------------- */

/* Events to consume per wakeup. Above the panel's rate, well below the
 * point where a full queue would keep the loop busy for long. */
#define EV_BATCH 64

static int swipe_y0, swipe_bright0, swipe_moved;

/*
 * The touch panel reports in its own coordinate range, not in display pixels.
 * On this phone fts_ts advertises ABS_MT_POSITION_X up to 10800 and
 * ABS_MT_POSITION_Y up to 24000, for a 1080x2400 panel - ten times finer than
 * the screen. Ask the device rather than assume a factor, since that ratio is a
 * property of this digitiser and not a law.
 */
static int touch_max_x = 1080, touch_max_y = 2400;


static void read_touch_range(int fd)
{
	struct input_absinfo ai;
	unsigned char buf[sizeof(ai)];

	if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), buf) == 0) {
		memcpy(&ai, buf, sizeof(ai));
		if (ai.maximum > 0)
			touch_max_x = ai.maximum;
	}
	if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), buf) == 0) {
		memcpy(&ai, buf, sizeof(ai));
		if (ai.maximum > 0)
			touch_max_y = ai.maximum;
	}

	fprintf(stderr, "lind: touch range x=%d y=%d (fd=%d)\n",
		touch_max_x, touch_max_y, fd);
}

/*
 * Persistent across calls, deliberately.
 *
 * This was local to read_touch() and so reinitialised on every wakeup. The
 * panel keeps reporting a finger down over many batches, but each batch started
 * with touching == 0, so the first batch set the origin and every batch after it
 * looked like the finger being lifted. A drag therefore never got past its
 * first movement.
 *
 * slot, last_x, last_y and touching are panel state and belong here. Only
 * have_abs is per-batch, and it is cleared once its SYN_REPORT is consumed -
 * it cannot be, because a SYN_REPORT may arrive in a later batch than the ABS
 * events it closes.
 */
struct touch_state {
	int have_abs;
	int slots_used;
	int last_x, last_y;
	int slot;
	int touching;
};

static struct touch_state ts;

static int read_touch(int fd, uint32_t mask, void *data)
{
	struct input_event ev;
	int n;

	(void)mask;
	(void)data;

	/*
	 * Bounded. A touch panel can report faster than this loop consumes,
	 * and an unbounded drain means the callback never returns, so the event
	 * loop never gets to anything else - the client socket is serviced by
	 * that same loop, so it would connect and then get nothing back. Read a
	 * batch and leave the rest for the next wakeup.
	 */
	for (n = 0; n < EV_BATCH; n++) {
		if (read(fd, &ev, sizeof(ev)) != sizeof(ev))
			break;
		drm.n_touch++;
		if (ev.type == EV_ABS) {
			ts.have_abs = 1;
			switch (ev.code) {
			case ABS_MT_SLOT:
				ts.slot = ev.value;
				if (ts.slot >= 16)
					ts.slot = 0;
				break;
			case ABS_MT_TRACKING_ID:
				if (ev.value >= 0) {
					ts.touching = 1;
				} else {
					ts.touching = 0;
				}
				break;
			/*
			 * This digitiser reports multi-touch protocol B only:
			 * getevent -p on fts_ts lists ABS_MT_POSITION_X/Y
			 * and no ABS_X/ABS_Y at all. Handling only the
			 * single-touch codes leaves every coordinate at zero,
			 * which is why the brightness gesture did nothing and
			 * the pointer had nowhere to go.
			 */
			case ABS_X:
			case ABS_MT_POSITION_X:
				ts.last_x = ev.value * (int)drm.w /
					    (touch_max_x ? touch_max_x : 1);
				break;
			case ABS_Y:
			case ABS_MT_POSITION_Y:
				ts.last_y = ev.value * (int)drm.h /
					    (touch_max_y ? touch_max_y : 1);
				break;
			}
		} else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
			if (ts.have_abs) {
				last_x = (uint32_t)ts.last_x;
				last_y = (uint32_t)ts.last_y;

				if (ts.touching && !touch_active) {
					/*
					 * Finger down. This has to happen
					 * before the drag below is
					 * applied, not after: it
					 * establishes the origin the
					 * drag is measured from, and
					 * clearing the moved flag
					 * afterwards meant the first
					 * movement of every swipe was
					 * always spent setting the
					 * origin - so a short swipe
					 * did nothing at all.
					 */
					swipe_y0 = ts.last_y;
					swipe_bright0 = bl_level;
					swipe_moved = 0;
					send_pointer(WL_POINTER_MOTION, 0, 0);
					/* Press as well as motion. Sending
					 * the button only on release meant
					 * the client saw "отпускание"
					 * and never a press, so a tap
					 * looked like it had started
					 * nowhere. */
					send_pointer(WL_POINTER_BUTTON,
						     BTN_LEFT,
						     WL_POINTER_BUTTON_STATE_PRESSED);
					touch_active = 1;
				} else if (ts.touching) {
					send_pointer(WL_POINTER_MOTION, 0, 0);

					/*
					 * Vertical drag sets brightness,
					 * as it does on Android. Absolute
					 * rather than incremental, so a
					 * drag that overshoots comes
					 * back instead of accumulating.
					 */
					if (bl_fd >= 0) {
						int dy = ts.last_y - swipe_y0;
						int span = (int)drm.h;

						if (dy > -10 && dy < 10)
							swipe_moved = 0;
						else if (span > 0) {
							bl_want = swipe_bright0 -
								 dy * bl_max /
								 span * 3 / 2;
							if (bl_want < bl_max / 10)
								bl_want = bl_max / 10;
							if (bl_want > bl_max)
								bl_want = bl_max;
							swipe_moved = 1;
						}
					}
				} else if (!ts.touching && touch_active) {
					swipe_moved = 0;
					send_pointer(WL_POINTER_BUTTON,
						     BTN_LEFT,
						     WL_POINTER_BUTTON_STATE_RELEASED);
					if (the_popup) {
						if (!inside_popup((int)last_x,
								  (int)last_y))
							dismiss_popup();
					} else if (wl_list_empty(&surfaces)) {
						int idx = tile_at((int)last_x,
								  (int)last_y);

						if (idx >= 0)
							launch(apps[idx].cmd);
					}
					touch_active = 0;
				}

				ts.have_abs = 0;
			}
		}
	}
	return 0;
}


/* --- xdg-shell ------------------------------------------------------------
 *
 * Everything an ordinary application needs to open a window, and nothing
 * more. A client creates a wl_surface, wraps it in an xdg_surface, puts an
 * xdg_toplevel inside, then attaches and commits - which is exactly what the
 * wl_surface path above already handles.
 *
 * Left out, deliberately rather than silently: xdg_popup, so menus and
 * subwindows will not open; window geometry, since a phone has one screen and
 * the client is given all of it; and focus, so every toplevel is treated
 * identically. None of that blocks a single full-screen client, which is the
 * case worth having.
 */

static const struct xdg_toplevel_interface toplevel_impl;

static void toplevel_destroy(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static void toplevel_set_parent(struct wl_client *c, struct wl_resource *r,
				struct wl_resource *parent)
{
	(void)c; (void)r; (void)parent;
}

static void toplevel_set_title(struct wl_client *c, struct wl_resource *r,
			       const char *title)
{
	(void)c;
	if (title)
		printf("lind: окно \"%s\"\n", title);
}

static void toplevel_set_app_id(struct wl_client *c, struct wl_resource *r,
				const char *app_id)
{
	(void)c; (void)r; (void)app_id;
}

static void toplevel_show_window_menu(struct wl_client *c,
				     struct wl_resource *r,
				     struct wl_resource *seat,
				     uint32_t serial, int32_t x, int32_t y)
{
	(void)c; (void)r; (void)seat; (void)serial; (void)x; (void)y;
}

static void toplevel_move(struct wl_client *c, struct wl_resource *r,
			  struct wl_resource *seat, uint32_t serial)
{
	(void)c; (void)r; (void)seat; (void)serial;
}

static void toplevel_resize(struct wl_client *c, struct wl_resource *r,
			    struct wl_resource *seat, uint32_t serial,
			    uint32_t edges)
{
	(void)c; (void)r; (void)seat; (void)serial; (void)edges;
}

static void toplevel_set_max_size(struct wl_client *c, struct wl_resource *r,
				  int32_t w, int32_t h)
{
	(void)c; (void)r; (void)w; (void)h;
}

static void toplevel_set_min_size(struct wl_client *c, struct wl_resource *r,
				  int32_t w, int32_t h)
{
	(void)c; (void)r; (void)w; (void)h;
}

static void toplevel_set_maximized(struct wl_client *c, struct wl_resource *r)
{
	struct surface *s = wl_resource_get_user_data(r);

	(void)c;
	/* One display means one maximum, which is the whole panel. */
	if (s)
		s->fullscreen_requested = 1;
}

static void toplevel_unset_maximized(struct wl_client *c, struct wl_resource *r)
{
	(void)c; (void)r;
}

static void toplevel_set_fullscreen(struct wl_client *c, struct wl_resource *r,
				    struct wl_resource *output)
{
	struct surface *s = wl_resource_get_user_data(r);

	(void)c; (void)output;
	if (s)
		s->fullscreen_requested = 1;
}

static void toplevel_unset_fullscreen(struct wl_client *c, struct wl_resource *r)
{
	(void)c; (void)r;
}

static void toplevel_set_minimized(struct wl_client *c, struct wl_resource *r)
{
	(void)c; (void)r;
}

static const struct xdg_toplevel_interface toplevel_impl = {
	.destroy = toplevel_destroy,
	.set_parent = toplevel_set_parent,
	.set_title = toplevel_set_title,
	.set_app_id = toplevel_set_app_id,
	.show_window_menu = toplevel_show_window_menu,
	.move = toplevel_move,
	.resize = toplevel_resize,
	.set_max_size = toplevel_set_max_size,
	.set_min_size = toplevel_set_min_size,
	.set_maximized = toplevel_set_maximized,
	.unset_maximized = toplevel_unset_maximized,
	.set_fullscreen = toplevel_set_fullscreen,
	.unset_fullscreen = toplevel_unset_fullscreen,
	.set_minimized = toplevel_set_minimized,
};

static void xdg_surface_destroy(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static void xdg_surface_get_toplevel(struct wl_client *c, struct wl_resource *r,
				     uint32_t id)
{
	struct surface *s = wl_resource_get_user_data(r);
	struct wl_resource *tl;

	if (!s)
		return;
	if (s->xdg_toplevel) {
		wl_resource_post_error(r, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,
				       "toplevel already created");
		return;
	}

	tl = wl_resource_create(c, &xdg_toplevel_interface,
				xdg_toplevel_interface.version, id);
	if (!tl)
		return;
	wl_resource_set_implementation(tl, &toplevel_impl, s, NULL);
	s->xdg_toplevel = tl;
	s->role = ROLE_TOPLEVEL;

	/*
	 * A client must not map until it has seen a configure, so this is the
	 * earliest point it can be sent. Doing it here rather than at commit
	 * time is not a detail: by commit the buffer is already applied.
	 */
	/*
	 * xdg_toplevel.configure carries a states array. Passing NULL is
	 * rejected by libwayland - "null value passed for arg 2" - and the
	 * client is dropped, which looks exactly like a compositor that maps
	 * nothing. An empty array is the correct way to say "no state
	 * changes", which is true here: the toplevel is given the whole panel.
	 */
	{
		struct wl_array states;

		wl_array_init(&states);
		xdg_toplevel_send_configure(s->xdg_toplevel, 0, 0, &states);
		wl_array_release(&states);
	}
}

static void popup_destroy(struct wl_client *c, struct wl_resource *r)
{
	struct surface *s = wl_resource_get_user_data(r);

	(void)c;
	if (s && s->xdg_popup == r) {
		if (the_popup == s)
			the_popup = NULL;
		s->xdg_popup = NULL;
		s->is_popup = 0;
		s->role = ROLE_NONE;
	}
}

/*
 * An input grab is optional for a client to request and we do not need it:
 * every click outside dismisses the popup anyway, which is the behaviour the
 * grab exists to provide. Ignoring it is safe because we are the only client on
 * this display.
 */
static void popup_grab(struct wl_client *c, struct wl_resource *r,
		       struct wl_resource *seat, uint32_t serial)
{
	(void)c; (void)r; (void)seat; (void)serial;
}

/* Repositioning is answered by configuring again, which is all a client waits
 * for before it redraws. */
static void popup_reposition(struct wl_client *c, struct wl_resource *r,
			     struct wl_resource *positioner, uint32_t token)
{
	struct surface *s = wl_resource_get_user_data(r);

	(void)c; (void)positioner;
	if (!s || !s->xdg_popup)
		return;

	if (s->current_buffer) {
		struct shm_buffer *b = wl_resource_get_user_data(s->current_buffer);

		xdg_popup_send_configure(s->xdg_popup, 0, 0,
					 b ? b->width : 0,
					 b ? b->height : 0);
	} else {
		xdg_popup_send_configure(s->xdg_popup, 0, 0, 0, 0);
	}
	/* No xdg_wm_base.done. It exists to close the initial burst of
	 * configures, and this compositor never sends one, so there is nothing
	 * to close - and the generated header here does not have it either,
	 * being older than the rest of the protocol. A client waiting for
	 * reposition waits on xdg_popup.repositioned, which is sent. */
}

static const struct xdg_popup_interface popup_impl = {
	.destroy = popup_destroy,
	.grab = popup_grab,
	.reposition = popup_reposition,
};

/*
 * xdg_positioner, which a popup cannot be created without.
 *
 * Its absence was not a missing feature so much as a missing object: the
 * interface table had destroy, get_xdg_surface and pong and no
 * create_positioner, so the very first thing any popup client does - ask for a
 * positioner - met a NULL request handler. libwayland's line for that is
 * "listener function for opcode 1 of xdg_wm_base is NULL", and it drops the
 * client, which presents as a compositor that hangs on the first popup.
 *
 * A positioner says where the popup goes relative to its parent's anchor
 * rectangle. Toplevels here are always the whole panel at the origin, so the
 * geometry reduces to placing the popup by its gravity relative to that anchor
 * rectangle - correct enough to put a menu where the client asked, without a
 * window manager's worth of geometry behind it.
 */
struct positioner {
	int32_t w, h;                    /* size */
	int32_t ax, ay, aw, ah;          /* anchor rectangle */
	int32_t ox, oy;                  /* offset */
	uint32_t anchor, gravity;
};

/*
 * Turning a positioner into a rectangle on the panel.
 *
 * The anchor says which point of the parent's anchor rectangle the popup
 * attaches to; the gravity says which point of the popup lands there. Both
 * are 3x3 grids, and composing them is nine cases rather than arithmetic -
 * the enum values are not a bit mask, so the obvious shift-and-mask is wrong
 * and silently gives LEFT a horizontal coordinate of "none".
 *
 * The parent is always the whole panel at the origin here, so the anchor
 * rectangle's own offset is the whole answer and no window geometry is
 * needed.
 */
static void popup_place(struct positioner *pos, int *out_x, int *out_y)
{
	int ax = pos->ax, ay = pos->ay;
	int x = 0, y = 0;

	/* Horizontal: anchor_x is the point in the parent, gravity_x shifts the
	 * popup so that its matching edge or middle meets it. */
	switch (pos->anchor) {
	case XDG_POSITIONER_ANCHOR_LEFT:
	case XDG_POSITIONER_ANCHOR_TOP_LEFT:
	case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT:
		ax = pos->ax;
		break;
	case XDG_POSITIONER_ANCHOR_RIGHT:
	case XDG_POSITIONER_ANCHOR_TOP_RIGHT:
	case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT:
		ax = pos->ax + pos->aw;
		break;
	case XDG_POSITIONER_ANCHOR_TOP:
	case XDG_POSITIONER_ANCHOR_BOTTOM:
		ax = pos->ax + pos->aw / 2;
		break;
	default:
		ax = pos->ax;
		break;
	}

	switch (pos->anchor) {
	case XDG_POSITIONER_ANCHOR_TOP:
	case XDG_POSITIONER_ANCHOR_TOP_LEFT:
	case XDG_POSITIONER_ANCHOR_TOP_RIGHT:
		ay = pos->ay;
		break;
	case XDG_POSITIONER_ANCHOR_BOTTOM:
	case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT:
	case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT:
		ay = pos->ay + pos->ah;
		break;
	case XDG_POSITIONER_ANCHOR_LEFT:
	case XDG_POSITIONER_ANCHOR_RIGHT:
		ay = pos->ay + pos->ah / 2;
		break;
	default:
		ay = pos->ay;
		break;
	}

	switch (pos->gravity) {
	case XDG_POSITIONER_GRAVITY_LEFT:
	case XDG_POSITIONER_GRAVITY_TOP_LEFT:
	case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT:
		x = ax;
		break;
	case XDG_POSITIONER_GRAVITY_RIGHT:
	case XDG_POSITIONER_GRAVITY_TOP_RIGHT:
	case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT:
		x = ax - pos->w;
		break;
	case XDG_POSITIONER_GRAVITY_TOP:
	case XDG_POSITIONER_GRAVITY_BOTTOM:
		x = ax - pos->w / 2;
		break;
	default:
		x = ax;
		break;
	}

	switch (pos->gravity) {
	case XDG_POSITIONER_GRAVITY_TOP:
	case XDG_POSITIONER_GRAVITY_TOP_LEFT:
	case XDG_POSITIONER_GRAVITY_TOP_RIGHT:
		y = ay;
		break;
	case XDG_POSITIONER_GRAVITY_BOTTOM:
	case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT:
	case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT:
		y = ay - pos->h;
		break;
	case XDG_POSITIONER_GRAVITY_LEFT:
	case XDG_POSITIONER_GRAVITY_RIGHT:
		y = ay - pos->h / 2;
		break;
	default:
		y = ay;
		break;
	}

	*out_x = x + pos->ox;
	*out_y = y + pos->oy;
}

static void xdg_surface_get_popup(struct wl_client *c, struct wl_resource *r,
				 uint32_t id, struct wl_resource *parent,
				 struct wl_resource *positioner)
{
	struct surface *s = wl_resource_get_user_data(r);
	struct wl_resource *pop;
	struct positioner *pos = wl_resource_get_user_data(positioner);

	if (!s)
		return;
	if (s->xdg_toplevel || s->xdg_popup) {
		wl_resource_post_error(r, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,
				       "surface already has a role");
		return;
	}

	/*
	 * One popup at a time. Asking for a second one replaces the first:
	 * clients that nest menus send this repeatedly and expect the older
	 * one to go away.
	 */
	dismiss_popup();

	pop = wl_resource_create(c, &xdg_popup_interface,
				 xdg_popup_interface.version, id);
	if (!pop)
		return;
	wl_resource_set_implementation(pop, &popup_impl, s, NULL);

	ui_dirty = 1;
	s->xdg_popup = pop;
	s->is_popup = 1;
	s->role = ROLE_POPUP;
	the_popup = s;

	if (parent && parent != r)
		s->popup_parent = parent;

	/* The positioner is the client's statement of where this goes, and it
	 * is fully parsed by now - every set_* request arrived before this
	 * get_popup. Honouring it is the difference between a menu under the
	 * button and a menu in the top-left corner. */
	if (pos) {
		s->popup_w = pos->w;
		s->popup_h = pos->h;
		popup_place(pos, &s->popup_x, &s->popup_y);

		/*
		 * Pull it back onto the panel.
		 *
		 * A positioner is a request, not a guarantee: it describes
		 * where the popup would go if nothing were in the way, and
		 * the compositor is obliged to keep it visible. Without this
		 * a menu hanging off the top edge - which is what an
		 * anchor rectangle near the top produces for a tall popup -
		 * is simply never seen, and the client is left waiting for
		 * input on something that is not there.
		 *
		 * Real constraint adjustment (flip, slide, resize) is not
		 * implemented; sliding is the one that preserves the client's
		 * size, so that is what this does. A popup taller than the
		 * panel is placed at the top rather than centred, because a
		 * negative coordinate cannot be blitted.
		 */
		if (s->popup_w && s->popup_w < drm.w)
			s->popup_x = CLAMP(s->popup_x, 0,
					   (int)drm.w - s->popup_w);
		else
			s->popup_x = 0;
		if (s->popup_h && s->popup_h < drm.h)
			s->popup_y = CLAMP(s->popup_y, 0,
					   (int)drm.h - s->popup_h);
		else
			s->popup_y = 0;
	} else {
		s->popup_w = s->popup_h = 0;
		s->popup_x = s->popup_y = 0;
	}
}

static void xdg_surface_set_window_geometry(struct wl_client *c,
					    struct wl_resource *r,
					    int32_t x, int32_t y,
					    int32_t w, int32_t h)
{
	struct surface *s = wl_resource_get_user_data(r);

	(void)c; (void)r; (void)x; (void)y;
	/* One screen, so a client that asks for a window at least as large as
	 * the panel is asking to fill it. Anything smaller is ignored rather
	 * than honoured, since there is nowhere to put the difference. */
	if (s && w > 0 && h > 0 && (uint32_t)w >= drm.w && (uint32_t)h >= drm.h)
		s->fullscreen_requested = 1;
}

static void xdg_surface_ack_configure(struct wl_client *c, struct wl_resource *r,
				      uint32_t serial)
{
	(void)c; (void)r; (void)serial;
}

static const struct xdg_surface_interface xdg_surface_impl = {
	.destroy = xdg_surface_destroy,
	.get_toplevel = xdg_surface_get_toplevel,
	.get_popup = xdg_surface_get_popup,
	.set_window_geometry = xdg_surface_set_window_geometry,
	.ack_configure = xdg_surface_ack_configure,
};


/* A ping obliges the client to answer with a pong. Sending a ping with no
 * handler for the reply is a protocol error the moment the client complies,
 * which is exactly what it did. */
static void wm_base_pong(struct wl_client *c, struct wl_resource *r,
			 uint32_t serial)
{
	(void)c; (void)r; (void)serial;
}

static void wm_base_destroy(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static void wm_base_get_xdg_surface(struct wl_client *c, struct wl_resource *r,
				   uint32_t id, struct wl_resource *surface_r)
{
	struct surface *s = wl_resource_get_user_data(surface_r);
	struct wl_resource *xs;

	if (!s) {
		wl_resource_post_error(r, XDG_WM_BASE_ERROR_ROLE,
				       "not a wl_surface");
		return;
	}

	if (s->xdg_surface) {
		wl_resource_post_error(r, XDG_WM_BASE_ERROR_ROLE,
				       "surface already has an xdg role");
		return;
	}

	xs = wl_resource_create(c, &xdg_surface_interface,
				xdg_surface_interface.version, id);
	if (!xs)
		return;
	wl_resource_set_implementation(xs, &xdg_surface_impl, s, NULL);
	s->xdg_surface = xs;
	s->role = ROLE_TOPLEVEL;
}

static void pos_destroy(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static void pos_set_size(struct wl_client *c, struct wl_resource *r,
			 int32_t width, int32_t height)
{
	struct positioner *p = wl_resource_get_user_data(r);

	(void)c;
	if (!p)
		return;
	if (width > 0)
		p->w = width;
	if (height > 0)
		p->h = height;
}

static void pos_set_anchor_rect(struct wl_client *c, struct wl_resource *r,
				int32_t x, int32_t y, int32_t width, int32_t height)
{
	struct positioner *p = wl_resource_get_user_data(r);

	(void)c;
	if (!p)
		return;
	p->ax = x;
	p->ay = y;
	p->aw = width;
	p->ah = height;
}

static void pos_set_anchor(struct wl_client *c, struct wl_resource *r,
			   uint32_t anchor)
{
	struct positioner *p = wl_resource_get_user_data(r);

	(void)c;
	if (p)
		p->anchor = anchor;
}

static void pos_set_gravity(struct wl_client *c, struct wl_resource *r,
			    uint32_t gravity)
{
	struct positioner *p = wl_resource_get_user_data(r);

	(void)c;
	if (p)
		p->gravity = gravity;
}

static void pos_set_constraint_adjustment(struct wl_client *c,
					  struct wl_resource *r,
					  uint32_t adj)
{
	(void)c; (void)r; (void)adj;
}

static void pos_set_offset(struct wl_client *c, struct wl_resource *r,
			   int32_t x, int32_t y)
{
	struct positioner *p = wl_resource_get_user_data(r);

	(void)c;
	if (!p)
		return;
	p->ox = x;
	p->oy = y;
}

static void pos_set_reactive(struct wl_client *c, struct wl_resource *r)
{
	(void)c; (void)r;
}

static void pos_set_parent_size(struct wl_client *c, struct wl_resource *r,
				int32_t w, int32_t h)
{
	(void)c; (void)r; (void)w; (void)h;
}

static void pos_set_parent_configure(struct wl_client *c, struct wl_resource *r,
				     uint32_t serial)
{
	(void)c; (void)r; (void)serial;
}

static const struct xdg_positioner_interface positioner_impl = {
	.destroy = pos_destroy,
	.set_size = pos_set_size,
	.set_anchor_rect = pos_set_anchor_rect,
	.set_anchor = pos_set_anchor,
	.set_gravity = pos_set_gravity,
	.set_constraint_adjustment = pos_set_constraint_adjustment,
	.set_offset = pos_set_offset,
	.set_reactive = pos_set_reactive,
	.set_parent_size = pos_set_parent_size,
	.set_parent_configure = pos_set_parent_configure,
};

static void wm_base_create_positioner(struct wl_client *c,
				      struct wl_resource *r, uint32_t id)
{
	struct positioner *p;
	struct wl_resource *pr;

	(void)r;

	p = xmalloc(sizeof(*p));
	if (!p)
		return;

	pr = wl_resource_create(c, &xdg_positioner_interface,
				xdg_positioner_interface.version, id);
	if (!pr) {
		free(p);
		return;
	}
	wl_resource_set_implementation(pr, &positioner_impl, p, NULL);
}

static const struct xdg_wm_base_interface wm_base_impl = {
	.destroy = wm_base_destroy,
	.create_positioner = wm_base_create_positioner,
	.get_xdg_surface = wm_base_get_xdg_surface,
	.pong = wm_base_pong,
};

static void bind_wm_base(struct wl_client *c, void *d, uint32_t ver, uint32_t id)
{
	struct wl_resource *r;

	(void)d;
	r = wl_resource_create(c, &xdg_wm_base_interface, ver, id);
	if (!r)
		return;
	wl_resource_set_implementation(r, &wm_base_impl, NULL, NULL);
	xdg_wm_base_send_ping(r, ++serial_counter);
}

/* --- globals ------------------------------------------------------------ */

static void bind_compositor(struct wl_client *c, void *d, uint32_t ver,
			    uint32_t id)
{
	struct wl_resource *r;

	(void)d;
	r = wl_resource_create(c, &wl_compositor_interface, ver, id);
	if (r)
		wl_resource_set_implementation(r, &compositor_impl, NULL, NULL);
}

static void bind_shm(struct wl_client *c, void *d, uint32_t ver, uint32_t id)
{
	struct wl_resource *r;

	(void)d;
	r = wl_resource_create(c, &wl_shm_interface, ver, id);
	if (!r)
		return;
	wl_resource_set_implementation(r, &shm_impl, NULL, NULL);
	{
		unsigned i;

		for (i = 0; i < sizeof(shm_formats) / sizeof(shm_formats[0]); i++)
			wl_shm_send_format(r, shm_formats[i]);
	}
}

static void bind_output(struct wl_client *c, void *d, uint32_t ver,
			uint32_t id)
{
	struct wl_resource *r;

	(void)d;
	r = wl_resource_create(c, &wl_output_interface, ver, id);
	if (!r)
		return;
	wl_resource_set_implementation(r, &output_impl, NULL, NULL);

	wl_output_send_geometry(r, 0, 0, 300, 150, WL_OUTPUT_SUBPIXEL_UNKNOWN,
				"lind", "panel", WL_OUTPUT_TRANSFORM_NORMAL);
	wl_output_send_mode(r, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
			    drm.w, drm.h, 120000);
	wl_output_send_scale(r, 1);
	wl_output_send_done(r);
}

static void bind_subcompositor(struct wl_client *c, void *d, uint32_t ver,
			       uint32_t id)
{
	struct wl_resource *r;

	(void)d;
	r = wl_resource_create(c, &wl_subcompositor_interface, ver, id);
	if (r)
		wl_resource_set_implementation(r, &subcompositor_impl, NULL,
					       NULL);
}

static void bind_seat(struct wl_client *c, void *d, uint32_t ver, uint32_t id)
{
	struct wl_resource *r;

	(void)d;
	r = wl_resource_create(c, &wl_seat_interface, ver, id);
	if (!r)
		return;
	wl_resource_set_implementation(r, &seat_impl, NULL, NULL);
	wl_seat_send_capabilities(r, WL_SEAT_CAPABILITY_POINTER);
	wl_seat_send_name(r, "seat0");
}

/* --- DRM setup ---------------------------------------------------------- */

static int drm_setup(void)
{
	drmModeRes *res;
	drmModeConnector *conn = NULL;
	int i;

	/*
	 * O_NONBLOCK is load-bearing, not tidiness.
	 *
	 * The DRM event fd is a packet stream, so the handler has to come back
	 * for the rest of a packet it has only half read. On a blocking fd that
	 * second read waits inside the event loop - and if the stream has
	 * desynchronised, or no further events are coming because the
	 * compositor has stopped presenting, it waits forever. The whole
	 * compositor then stops: the screen freezes and touch dies with it,
	 * which is exactly what it looks like from the outside.
	 *
	 * It used to be safe to leave this blocking only because the handler
	 * read once per poll, and poll had already promised data.
	 */
	drm.fd = open(CARD, O_RDWR | O_CLOEXEC | O_NONBLOCK);
	if (drm.fd < 0) {
		fprintf(stderr, "lind: open %s: %s\n", CARD, strerror(errno));
		return -1;
	}
	if (drmSetMaster(drm.fd) < 0 && errno != EINVAL) {
		fprintf(stderr, "lind: drmSetMaster: %s\n", strerror(errno));
		return -1;
	}
	printf("lind: DRM master на %s\n", CARD);

	res = drmModeGetResources(drm.fd);
	if (!res)
		return -1;

	for (i = 0; i < res->count_connectors; i++) {
		conn = drmModeGetConnector(drm.fd, res->connectors[i]);
		if (!conn)
			continue;
		if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes)
			break;
		drmModeFreeConnector(conn);
		conn = NULL;
	}
	if (!conn) {
		fprintf(stderr, "lind: нет подключённого коннектора\n");
		drmModeFreeResources(res);
		return -1;
	}
	drm.conn_id = conn->connector_id;
	drm.crtc_id = res->crtcs[0];
	drm.w = conn->modes[0].hdisplay;
	drm.h = conn->modes[0].vdisplay;
	printf("lind: коннектор %u, CRTC %u, %ux%u\n", drm.conn_id,
	       drm.crtc_id, drm.w, drm.h);

	for (i = 0; i < FB_COUNT; i++) {
		uint32_t hs[4] = { 0 }, ps[4] = { 0 }, os[4] = { 0 };

		if (drmModeCreateDumbBuffer(drm.fd, drm.w, drm.h, 32, 0,
					    &drm.handle[i], &drm.stride[i],
					    &drm.size) < 0) {
			fprintf(stderr, "lind: dumb buffer: %s\n",
				strerror(errno));
			goto fail;
		}
		hs[0] = drm.handle[i];
		ps[0] = drm.stride[i];
		if (drmModeAddFB2(drm.fd, drm.w, drm.h, DRM_FORMAT_XRGB8888,
				  hs, ps, os, &drm.fb[i], 0) < 0) {
			fprintf(stderr, "lind: AddFB2: %s\n", strerror(errno));
			goto fail;
		}
	}

	for (i = 0; i < FB_COUNT; i++) {
		uint64_t off = 0;

		if (drmModeMapDumbBuffer(drm.fd, drm.handle[i], &off) < 0) {
			fprintf(stderr, "lind: map dumb %d: %s\n", i,
				strerror(errno));
			goto fail;
		}
		drm.map[i] = mmap(NULL, drm.size, PROT_READ | PROT_WRITE,
				  MAP_SHARED, drm.fd, off);
		if (drm.map[i] == MAP_FAILED) {
			drm.map[i] = NULL;
			fprintf(stderr, "lind: mmap %d: %s\n", i,
				strerror(errno));
			goto fail;
		}
	}

	drm.render = 0;
	drm.scanout = 0;

	if (drmModeSetCrtc(drm.fd, drm.crtc_id, drm.fb[0], 0, 0,
			   &drm.conn_id, 1, &conn->modes[0]) < 0) {
		fprintf(stderr, "lind: SetCrtc: %s\n", strerror(errno));
		goto fail;
	}
	drm.mode_set = 1;

	/* Armed here, before anything else, so the driver has a vblank
	 * reference from the first moment and its frame-boundary interrupt is
	 * live before the first page flip is asked for. Arming later leaves a
	 * window where a flip can complete with nothing watching. */
	vblank_arm();

	printf("lind: режим установлен, буфер отображения готов\n");

	drmModeFreeConnector(conn);
	drmModeFreeResources(res);
	return 0;

fail:
	if (conn)
		drmModeFreeConnector(conn);
	drmModeFreeResources(res);
	return -1;
}

static void drm_teardown(void)
{
	int i;

	for (i = 0; i < FB_COUNT; i++) {
		if (drm.map[i])
			munmap(drm.map[i], drm.size);
		if (drm.fb[i])
			drmModeRmFB(drm.fd, drm.fb[i]);
		if (drm.handle[i])
			drmModeDestroyDumbBuffer(drm.fd, drm.handle[i]);
	}
	if (drm.fd >= 0) {
		drmDropMaster(drm.fd);
		close(drm.fd);
		drm.fd = -1;
	}
}

static struct wl_display *the_display;

static void on_signal(int sig)
{
	dump_stats();
	(void)sig;
	/* wl_display_run() has no way out, so the signal handler has to end
	 * it from the inside. */
	if (the_display)
		wl_display_terminate(the_display);
}

int main(void)
{
	struct wl_display *display;
	struct wl_event_loop *loop;
	int touch_fd;

	wl_list_init(&surfaces);
	wl_list_init(&pointers);

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	/* Wayland insists on a valid XDG_RUNTIME_DIR, and inside the chroot it
	 * is rarely set. Do it here rather than relying on the caller. */
	setenv("XDG_RUNTIME_DIR", RUNTIME_DIR, 1);
	mkdir(RUNTIME_DIR, 0700);

	if (drm_setup() < 0)
		return 1;

	display = wl_display_create();
	if (!display) {
		fprintf(stderr, "lind: wl_display_create не удался\n");
		drm_teardown();
		return 1;
	}

	{
		/* add_socket_auto picks a free name; fall back to a fixed
		 * one so clients can be pointed at it. */
		const char *sock = wl_display_add_socket_auto(display);

		if (!sock) {
			if (wl_display_add_socket(display, SOCKET_NAME) < 0) {
				fprintf(stderr, "lind: сокет не создался\n");
				wl_display_destroy(display);
				drm_teardown();
				return 1;
			}
		}
	}

	wl_global_create(display, &wl_compositor_interface, 4, NULL,
			 bind_compositor);
	wl_global_create(display, &wl_shm_interface, 1, NULL, bind_shm);
	wl_global_create(display, &wl_subcompositor_interface, 1, NULL,
			 bind_subcompositor);
	wl_global_create(display, &wl_output_interface, 2, NULL, bind_output);
	wl_global_create(display, &wl_seat_interface, 5, NULL, bind_seat);
	wl_global_create(display, &xdg_wm_base_interface, 3, NULL, bind_wm_base);

	loop = wl_display_get_event_loop(display);



	wl_event_loop_add_fd(loop, drm.fd, WL_EVENT_READABLE, on_drm_event,
			     NULL);

	/* Android's SurfaceFlinger normally owns this. Once we take the
	 * display the panel keeps whatever level it last had, 315 of 2047
	 * on this phone, which reads as a dim screen rather than as a
	 * compositor problem. -1 means full. */
	brightness_init(-1);

	if (text_init())
		fprintf(stderr, "lind: шрифт не загрузился, подписей нет\n");
	load_apps();
	fprintf(stderr, "lind: приложений в сетке: %d\n", n_apps);

	pace_timer = wl_event_loop_add_timer(loop, pace_dispatch, NULL);
	if (!pace_timer) {
		fprintf(stderr, "lind: не создался таймер частоты\n");
		wl_display_destroy(display);
		drm_teardown();
		return 1;
	}

	/*
	 * Arm it here, after the timer exists - which is the whole point. Armed
	 * one step earlier this tested a NULL handle and did nothing, and with
	 * no client there is no commit to arm it later: repaint=0, frames=0,
	 * and a bare gradient that nothing would ever redraw.
	 */
	arm_keepalive();

	touch_fd = open(TOUCH_DEV, O_RDONLY | O_NONBLOCK);
	read_touch_range(touch_fd);
	if (touch_fd < 0)
		fprintf(stderr, "lind: %s: %s (тач не будет)\n", TOUCH_DEV,
			strerror(errno));
	else
		wl_event_loop_add_fd(loop, touch_fd, WL_EVENT_READABLE,
				     read_touch, NULL);

	fill_background();
	printf("lind: слушаю Wayland на %s; Ctrl-C или 'lindroid back' "
	       "для выхода\n", SOCKET_NAME);
	fflush(stdout);

	/*
	 * wl_display_run(), not a hand-rolled loop. Dispatching the event loop
	 * by hand does not poll the display's own listening socket, so clients
	 * connect and then nothing happens: no globals are ever sent, and even
	 * an internal timer never fires. wl_display_run() adds that fd, flushes
	 * clients and dispatches, which is the whole server.
	 */
	the_display = display;
	wl_display_run(display);
	the_display = NULL;

	printf("lind: выхожу\n");
	if (touch_fd >= 0)
		close(touch_fd);
	wl_display_destroy(display);
	drm_teardown();
	return 0;
}
