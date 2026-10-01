// SPDX-License-Identifier: GPL-2.0-only
/*
 * lindtest - a client for lind, to prove the render path end to end.
 *
 * Creates a surface, allocates a wl_shm buffer, draws a moving bar into it and
 * commits, driven by frame callbacks. If this shows the bar travelling across
 * the panel then the whole path works: evdev is not involved, the compositor
 * blits the client buffer into the scanned-out dumb buffer, and the panel
 * follows.
 *
 * This is deliberately a raw client rather than a weston test, because the
 * point is to need nothing beyond wl_compositor and wl_shm. lind implements
 * neither xdg-shell nor wl_shell, so every convenience client in the chroot
 * would fail to map a window for unrelated reasons.
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -O2 -I<rootfs>/usr/include lindtest.c \
 *         -o lindtest -L<rootfs>/usr/lib/aarch64-linux-gnu -lwayland-client
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

static struct wl_compositor *compositor;
static struct xdg_wm_base *wm_base;
static struct wl_shm *shm;
static struct wl_output *output;
static struct wl_seat *seat;
static struct wl_pointer *pointer;

static int width = 1080, height = 2400;
static int have_output_info;

static void shm_format(void *d, struct wl_shm *s, uint32_t format)
{
	(void)d; (void)s; (void)format;
}

static const struct wl_shm_listener shm_listener_impl = {
	.format = shm_format,
};

static void output_geometry(void *d, struct wl_output *o, int32_t x, int32_t y,
			    int32_t pw, int32_t ph, int32_t sub, const char *make,
			    const char *model, int32_t transform)
{
	(void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sub;
	(void)make; (void)transform;
	printf("выход: %s\n", model);
}

static void output_mode(void *d, struct wl_output *o, uint32_t flags, int32_t w,
			int32_t h, int32_t refresh)
{
	(void)d; (void)o; (void)flags;
	printf("режим: %dx%d @ %.1f Гц\n", w, h, refresh / 1000.0);
	width = w;
	height = h;
	have_output_info = 1;
}

static void output_done(void *d, struct wl_output *o)
{
	(void)d; (void)o;
}

static void output_scale(void *d, struct wl_output *o, int32_t factor)
{
	(void)d; (void)o; (void)factor;
}

static const struct wl_output_listener output_impl = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
};

static void seat_capabilities(void *d, struct wl_seat *s, uint32_t caps)
{
	(void)d; (void)s; (void)caps;
}

static void seat_name(void *d, struct wl_seat *s, const char *name)
{
	(void)d; (void)s;
	printf("seat: %s\n", name);
}

static const struct wl_seat_listener seat_impl = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

/*
 * Touch is now correct end to end: multi-touch protocol B codes, panel range
 * scaled from 10800x24000 to display pixels, and state that survives between
 * event batches. Before that the compositor sent (0,0) and nothing here could
 * tell the difference between a working pointer and a dead one.
 *
 * So the test now draws where the finger is. A pointer that moves but leaves no
 * trace is indistinguishable from one that is not delivered, which is exactly
 * the ambiguity that hid the earlier faults.
 */
static int touch_x = -1, touch_y = -1;
static int touch_active, touch_seen;
static unsigned long touch_presses;

static void pointer_enter(void *d, struct wl_pointer *p, uint32_t serial,
			  struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
	(void)d; (void)p; (void)s; (void)serial;
	touch_x = wl_fixed_to_int(x);
	touch_y = wl_fixed_to_int(y);
	touch_seen = 1;
}

static void pointer_leave(void *d, struct wl_pointer *p, uint32_t serial,
			  struct wl_surface *s)
{
	(void)d; (void)p; (void)s; (void)serial;
}

static void pointer_motion(void *d, struct wl_pointer *p, uint32_t serial,
			   wl_fixed_t x, wl_fixed_t y)
{
	(void)d; (void)p; (void)serial;
	touch_x = wl_fixed_to_int(x);
	touch_y = wl_fixed_to_int(y);
	touch_seen = 1;
}

static void pointer_button(void *d, struct wl_pointer *p, uint32_t serial,
			   uint32_t time, uint32_t button, uint32_t state)
{
	(void)d; (void)p; (void)serial; (void)time; (void)button;

	/* state is the button state: 0 pressed, 1 released. */
	touch_active = !state;
	touch_seen = 1;
	if (!state)
		touch_presses++;
	printf("нажатие в (%d,%d): %s, всего %lu\n", touch_x, touch_y,
	       state ? "отпускание" : "нажата", touch_presses);
}

static void pointer_axis(void *d, struct wl_pointer *p, uint32_t time,
			 uint32_t axis, wl_fixed_t value)
{
	(void)d; (void)p; (void)time; (void)axis; (void)value;
}

/*
 * Every event needs a listener, including the ones this test has no use for.
 * A NULL slot is not "ignored": libwayland logs "listener function for opcode
 * N of wl_pointer is NULL" and the client stops, which reads as the compositor
 * hanging rather than as a missing stub.
 *
 * Opcode 5 is frame - the one that actually arrived here. Signatures are taken
 * from wayland-client-protocol.h as it exists in this tree, which is older than
 * current: axis_discrete carries (axis, discrete) with no time argument, and
 * frame takes no time either.
 */
static void pointer_frame(void *d, struct wl_pointer *p)
{
	(void)d; (void)p;
}

static void pointer_axis_source(void *d, struct wl_pointer *p, uint32_t src)
{
	(void)d; (void)p; (void)src;
}

static void pointer_axis_stop(void *d, struct wl_pointer *p, uint32_t t,
		uint32_t a)
{
	(void)d; (void)p; (void)t; (void)a;
}

static void pointer_axis_discrete(void *d, struct wl_pointer *p, uint32_t a,
		int32_t v)
{
	(void)d; (void)p; (void)a; (void)v;
}

static void pointer_axis_value120(void *d, struct wl_pointer *p, uint32_t a,
		int32_t v)
{
	(void)d; (void)p; (void)a; (void)v;
}

static const struct wl_pointer_listener pointer_impl = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
	.axis_value120 = pointer_axis_value120,
};

static void wm_base_ping(void *d, struct xdg_wm_base *b, uint32_t serial)
{
	(void)d;
	xdg_wm_base_pong(b, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

static struct xdg_toplevel *top;
static int configured;

static void surface_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{
	(void)d;
	xdg_surface_ack_configure(xs, serial);
	configured = 1;
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = surface_configure,
};

static void toplevel_configure(void *d, struct xdg_toplevel *t, int32_t w,
			       int32_t h, struct wl_array *states)
{
	(void)d; (void)t; (void)w; (void)h; (void)states;
	configured = 1;
}

static void toplevel_close(void *d, struct xdg_toplevel *t)
{
	(void)d; (void)t;
}

static void toplevel_configure_bounds(void *d, struct xdg_toplevel *t,
				      int32_t w, int32_t h)
{
	(void)d; (void)t; (void)w; (void)h;
}

static void toplevel_wm_capabilities(void *d, struct xdg_toplevel *t,
				     struct wl_array *caps)
{
	(void)d; (void)t; (void)caps;
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
	.configure_bounds = toplevel_configure_bounds,
	.wm_capabilities = toplevel_wm_capabilities,
};

static void registry_global(void *d, struct wl_registry *r, uint32_t name,
			    const char *iface, uint32_t version)
{
	(void)d;
	if (!strcmp(iface, "wl_compositor")) {
		compositor = wl_registry_bind(r, name, &wl_compositor_interface,
					      version < 4 ? version : 4);
	} else if (!strcmp(iface, "wl_shm")) {
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
		wl_shm_add_listener(shm, &shm_listener_impl, NULL);
	} else if (!strcmp(iface, "wl_output") && !output) {
		output = wl_registry_bind(r, name, &wl_output_interface,
					  version < 2 ? version : 2);
		wl_output_add_listener(output, &output_impl, NULL);
	} else if (!strcmp(iface, "xdg_wm_base") && !wm_base) {
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface,
					   version < 3 ? version : 3);
		xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
	} else if (!strcmp(iface, "wl_seat") && !seat) {
		seat = wl_registry_bind(r, name, &wl_seat_interface,
				       version < 5 ? version : 5);
		wl_seat_add_listener(seat, &seat_impl, NULL);
	}
}

static void registry_remove(void *d, struct wl_registry *r, uint32_t name)
{
	(void)d; (void)r; (void)name;
}

static const struct wl_registry_listener registry_impl = {
	.global = registry_global,
	.global_remove = registry_remove,
};

static void frame_done(void *d, struct wl_callback *cb, uint32_t time)
{
	(void)d; (void)time;
	wl_callback_destroy(cb);
}

static const struct wl_callback_listener frame_impl = {
	.done = frame_done,
};

/*
 * The test pattern answers three questions at once, because getting any of
 * them wrong looks like "the compositor is not drawing":
 *
 *   - orientation: a white square pinned to the top left of the buffer. If it
 *     appears elsewhere on the panel, the buffer is being rotated.
 *   - channel order: solid red, green and blue blocks across the top. If they
 *     come out as other hues, the format is not what we assumed.
 *   - motion: a red bar travelling down, a blue bar travelling right, each with
 *     its own parameter scaled to its own axis.
 *
 * The previous version shared one parameter between a vertical and a
 * horizontal bar, and used it as x while scaling it to height. On a 1080x2400
 * panel that put the "vertical" bar off-screen for the last 55% of the run,
 * which reads as the animation stalling.
 */
static void fill_rect(void *pixels, int stride, int x0, int y0, int w, int h,
		      uint32_t colour)
{
	int x, y;

	for (y = y0; y < y0 + h; y++) {
		uint32_t *row;

		if (y < 0 || y >= height)
			continue;
		row = (uint32_t *)((char *)pixels + (size_t)y * stride);
		for (x = x0; x < x0 + w; x++) {
			if (x >= 0 && x < width)
				row[x] = colour;
		}
	}
}

static void draw(struct wl_surface *surface, struct wl_buffer *buffer,
		 void *pixels, int stride, int ypos, int xpos)
{
	int y, x;

	for (y = 0; y < height; y++) {
		uint32_t *row = (uint32_t *)((char *)pixels + (size_t)y * stride);

		for (x = 0; x < width; x++)
			row[x] = 0xFF181818;
	}

	/* Orientation marker. */
	fill_rect(pixels, stride, 0, 0, 120, 120, 0xFFFFFFFF);

	/* Channel-order probe. */
	fill_rect(pixels, stride, 130, 0, 60, 60, 0xFFFF0000);   /* red */
	fill_rect(pixels, stride, 200, 0, 60, 60, 0xFF00FF00);   /* green */
	fill_rect(pixels, stride, 270, 0, 60, 60, 0xFF0000FF);   /* blue */

	/* Red bar: travels down, scaled to height. */
	fill_rect(pixels, stride, 350, ypos - 20, 40, 40, 0xFFFF0000);

	/* Blue bar: travels right, scaled to width. */
	fill_rect(pixels, stride, xpos - 20, 200, 40, 40, 0xFF0000FF);

	/*
	 * Where the finger is: a filled disc while held, a ring after
	 * release, so a tap leaves a mark that fades rather than vanishing.
	 */
	if (touch_seen) {
		int r = 60, inner = (r - 12) * (r - 12);

		for (y = touch_y - r; y <= touch_y + r; y++) {
			uint32_t *trow;

			if (y < 0 || y >= height)
				continue;
			trow = (uint32_t *)((char *)pixels + (size_t)y * stride);
			for (x = touch_x - r; x <= touch_x + r; x++) {
				int dx = x - touch_x, dy = y - touch_y;
				int d2 = dx * dx + dy * dy;

				if (x < 0 || x >= width || d2 > r * r)
					continue;
				if (d2 > inner)
					trow[x] = 0xFFFFFFFF;
				else
					trow[x] = touch_active ? 0xFF60FF60
							       : 0xFF707070;
			}
		}
	}

	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage(surface, 0, 0, width, height);
	wl_surface_commit(surface);
}

int main(int argc, char **argv)
{
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_surface *surface;
	struct wl_buffer *buffer;
	struct wl_shm_pool *pool;
	int stride = width * 4, size = stride * height;
	int frames = 0, seconds = argc > 1 ? atoi(argv[1]) : 10;
	int fd;
	void *pixels;

	display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "не подключиться: %s\n", strerror(errno));
		return 1;
	}
	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_impl, NULL);
	wl_display_roundtrip(display);
	wl_display_roundtrip(display);

	if (!compositor || !shm || !wm_base) {
		fprintf(stderr,
			"нет wl_compositor/wl_shm/xdg_wm_base\n");
		return 1;
	}
	if (!have_output_info) {
		fprintf(stderr, "нет wl_output\n");
		return 1;
	}
	stride = width * 4;
	size = stride * height;

	/*
	 * The pool fd belongs to this process, so mmap it directly rather
	 * than through a libdrm helper. This version of libwayland has no
	 * wl_shm_pool_get_data, and the server sends no fd back anyway - the
	 * client already holds the only copy that matters.
	 */
	fd = memfd_create("lind-buffer", 0);
	if (fd < 0) {
		fprintf(stderr, "memfd_create: %s\n", strerror(errno));
		return 1;
	}
	if (ftruncate(fd, (off_t)size) < 0) {
		fprintf(stderr, "ftruncate(%d): %s\n", (int)size,
			strerror(errno));
		return 1;
	}
	pixels = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED,
		      fd, 0);
	if (pixels == MAP_FAILED) {
		fprintf(stderr, "mmap: %s\n", strerror(errno));
		return 1;
	}

	/*
	 * A window the xdg way: wl_surface, wrapped in an xdg_surface, with an
	 * xdg_toplevel inside. A client is not allowed to map until the
	 * compositor has sent a configure, so the roundtrip after creating the
	 * toplevel is required, not decorative.
	 */
	surface = wl_compositor_create_surface(compositor);
	{
		struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base,
								  surface);

		xdg_surface_add_listener(xs, &xdg_surface_listener, NULL);
		top = xdg_surface_get_toplevel(xs);
		xdg_toplevel_add_listener(top, &toplevel_listener, NULL);
		xdg_toplevel_set_title(top, "lindtest");
		xdg_toplevel_set_app_id(top, "lind.test");
		xdg_toplevel_set_fullscreen(top, NULL);
		wl_surface_commit(surface);
	}
	wl_display_roundtrip(display);
	if (!configured) {
		fprintf(stderr, "композитор не прислал configure\n");
		return 1;
	}
	printf("окно смаплено через xdg-shell\n");

	pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
					   WL_SHM_FORMAT_XRGB8888);

	if (seat) {
		pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(pointer, &pointer_impl, NULL);
	}

	printf("рисую %d с, %dx%d\n", seconds, width, height);
	fflush(stdout);

	{
		struct timespec start, ts;
		int ypos = 0, xpos = 0, elapsed = 0;

		clock_gettime(CLOCK_MONOTONIC, &start);
		while (1) {
			struct wl_callback *cb;

			clock_gettime(CLOCK_MONOTONIC, &ts);
			if ((int)((ts.tv_sec - start.tv_sec) * 1000 +
				  (ts.tv_nsec - start.tv_nsec) / 1000000) >
			    seconds * 1000)
				break;

			elapsed = (int)((ts.tv_sec - start.tv_sec) * 1000 +
					(ts.tv_nsec - start.tv_nsec) / 1000000);
			/* Each bar scales to its own axis. Sharing one value
			 * put the horizontal bar off-screen after 45%. */
			ypos = elapsed * height / (seconds * 1000);
			xpos = elapsed * width / (seconds * 1000);
			if (ypos < 0) ypos = 0;
			if (ypos >= height) ypos = height - 1;
			if (xpos < 0) xpos = 0;
			if (xpos >= width) xpos = width - 1;

			cb = wl_surface_frame(surface);
			wl_callback_add_listener(cb, &frame_impl, NULL);
			draw(surface, buffer, pixels, stride, ypos, xpos);

			wl_display_roundtrip(display);
			frames++;
			if (frames % 10 == 0) {
				printf("  кадр %d, красная y=%d, синяя x=%d\n",
				       frames, ypos, xpos);
				fflush(stdout);
			}
		}
	}

	printf("итого %d кадров\n", frames);
	wl_buffer_destroy(buffer);
	wl_shm_pool_destroy(pool);
	xdg_toplevel_destroy(top);
	wl_surface_destroy(surface);
	wl_display_disconnect(display);
	return 0;
}
