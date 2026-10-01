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

static struct wl_compositor *compositor;
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

static void pointer_enter(void *d, struct wl_pointer *p, uint32_t serial,
			  struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
	(void)d; (void)p; (void)s; (void)x; (void)y;
	(void)serial;
}

static void pointer_leave(void *d, struct wl_pointer *p, uint32_t serial,
			  struct wl_surface *s)
{
	(void)d; (void)p; (void)s; (void)serial;
}

static void pointer_motion(void *d, struct wl_pointer *p, uint32_t serial,
			   wl_fixed_t x, wl_fixed_t y)
{
	(void)d; (void)p; (void)x; (void)y;
	(void)serial;
}

static void pointer_button(void *d, struct wl_pointer *p, uint32_t serial,
			   uint32_t time, uint32_t button, uint32_t state)
{
	(void)d; (void)p; (void)serial; (void)time; (void)button;
	printf("нажатие: %s\n", state ? "отпускание" : "нажата");
}

static void pointer_axis(void *d, struct wl_pointer *p, uint32_t time,
			 uint32_t axis, wl_fixed_t value)
{
	(void)d; (void)p; (void)time; (void)axis; (void)value;
}

static const struct wl_pointer_listener pointer_impl = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = NULL,
	.axis_source = NULL,
	.axis_stop = NULL,
	.axis_discrete = NULL,
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

static void draw(struct wl_surface *surface, struct wl_buffer *buffer,
		 void *pixels, int stride, int bar)
{
	int y, x;

	/* Dark background, one bright bar, plus a marker at the top so a
	 * partially updated screen is obvious. */
	for (y = 0; y < height; y++) {
		uint32_t *row = (uint32_t *)((char *)pixels + (size_t)y * stride);

		for (x = 0; x < width; x++)
			row[x] = 0xFF202020;
	}
	for (y = 0; y < height; y += 8) {
		uint32_t *row = (uint32_t *)((char *)pixels + (size_t)y * stride);

		for (x = bar - 20; x < bar + 20; x++)
			if (x >= 0 && x < width)
				row[x] = 0xFF00FF00;
	}
	for (x = 0; x < width; x += 16) {
		uint32_t *row = (uint32_t *)((char *)pixels + bar * stride);

		if (bar < height)
			row[x] = 0xFFFFFF00;
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

	if (!compositor || !shm) {
		fprintf(stderr, "нет wl_compositor/wl_shm\n");
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

	surface = wl_compositor_create_surface(compositor);
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
		int bar = 0;

		clock_gettime(CLOCK_MONOTONIC, &start);
		while (1) {
			struct wl_callback *cb;

			clock_gettime(CLOCK_MONOTONIC, &ts);
			if ((int)((ts.tv_sec - start.tv_sec) * 1000 +
				  (ts.tv_nsec - start.tv_nsec) / 1000000) >
			    seconds * 1000)
				break;

			bar = (int)(((ts.tv_sec - start.tv_sec) * 1000 +
				     (ts.tv_nsec - start.tv_nsec) / 1000000) *
				    width / (seconds * 1000));
			if (bar < 0)
				bar = 0;
			if (bar >= height)
				bar = height - 1;

			cb = wl_surface_frame(surface);
			wl_callback_add_listener(cb, &frame_impl, NULL);
			draw(surface, buffer, pixels, stride, bar);

			wl_display_roundtrip(display);
			frames++;
			if (frames % 10 == 0) {
				printf("  кадр %d, полоса y=%d\n", frames, bar);
				fflush(stdout);
			}
		}
	}

	printf("итого %d кадров\n", frames);
	wl_buffer_destroy(buffer);
	wl_shm_pool_destroy(pool);
	wl_surface_destroy(surface);
	wl_display_disconnect(display);
	return 0;
}
