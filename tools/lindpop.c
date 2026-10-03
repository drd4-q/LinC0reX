/*
 * lindpop - a client that asks for an xdg_popup, and nothing else.
 *
 * Exists to answer one question: does lind's popup path work end to end? A
 * compositor can advertise xdg_wm_base, serve toplevels perfectly and still
 * reject popups - which is what lind did until recently, with a
 * NOT_CONSTRUCTED error and the client dropped.
 *
 * The sequence a popup forces, which a toplevel does not:
 *   create xdg_positioner -> xdg_surface.get_popup(parent, positioner)
 *   -> attach + commit -> wait for xdg_popup.configure -> ack -> commit
 *
 * A client that skips the wait gets its connection closed, so this blocks in
 * roundtrip until the configure arrives, then draws.
 *
 * Usage: lindpop [seconds]
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wl_surface *surf;
static struct xdg_surface *xdg_surf;
static struct xdg_surface *parent_xdg;
static struct xdg_popup *popup;
static int configured, dismissed;
static int cfg_x, cfg_y;
static int width = 320, height = 240;

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial)
{
	(void)d; (void)b;
	xdg_wm_base_pong(b, serial);
}

static const struct xdg_wm_base_listener wm_impl = { .ping = wm_ping };

static void popup_configure(void *d, struct xdg_popup *p,
			    int32_t x, int32_t y, int32_t w, int32_t h)
{
	(void)d; (void)p;

	cfg_x = x;
	cfg_y = y;
	if (w > 0)
		width = w;
	if (h > 0)
		height = h;
	configured = 1;
}

static void popup_done(void *d, struct xdg_popup *p)
{
	(void)d; (void)p;
	printf("lindpop: композитор закрыл popup_done\n");
	dismissed = 1;
}

static const struct xdg_popup_listener popup_impl = {
	.configure = popup_configure,
	.popup_done = popup_done,
};

static void xdg_surface_configure(void *d, struct xdg_surface *s, uint32_t serial)
{
	(void)d;
	xdg_surface_ack_configure(s, serial);
}

static const struct xdg_surface_listener xdg_impl = {
	.configure = xdg_surface_configure,
};

static void registry_global(void *d, struct wl_registry *r, uint32_t name,
			    const char *iface, uint32_t ver)
{
	(void)d;

	if (!strcmp(iface, "wl_compositor"))
		comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	else if (!strcmp(iface, "wl_shm"))
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, "xdg_wm_base"))
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
}

/* wl_registry_listener has no remove member in this libwayland - it arrived
 * with registry version 2, and this tree binds version 1. */
static const struct wl_registry_listener reg_impl = {
	.global = registry_global,
};

/* memfd_create needs _GNU_SOURCE, which is not worth setting for a whole file;
 * the syscall is declared here instead. */
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

static int memfd_create(const char *name, unsigned int flags)
{
	return (int)syscall(SYS_memfd_create, name, flags);
}

static void *alloc_pixels(size_t size, int *fd_out)
{
	void *p;
	int fd = memfd_create("lindpop", MFD_CLOEXEC);

	if (fd < 0)
		return NULL;
	if (ftruncate(fd, (off_t)size) < 0)
		return NULL;
	p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED)
		return NULL;
	*fd_out = fd;
	return p;
}

int main(int argc, char **argv)
{
	struct wl_display *dpy;
	struct wl_registry *reg;
	struct xdg_positioner *pos;
	struct wl_buffer *buf;
	struct wl_shm_pool *pool;
	uint32_t *px;
	int stride, size, fd = -1;
	int secs = argc > 1 ? atoi(argv[1]) : 5;
	time_t end;

	dpy = wl_display_connect(NULL);
	if (!dpy) {
		fprintf(stderr, "lindpop: не подключиться к Wayland\n");
		return 1;
	}

	reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_impl, NULL);
	wl_display_roundtrip(dpy);

	if (!comp || !shm || !wm_base) {
		fprintf(stderr, "lindpop: нет wl_compositor, wl_shm или xdg_wm_base\n");
		return 1;
	}
	xdg_wm_base_add_listener(wm_base, &wm_impl, NULL);

	/* A toplevel first: the protocol will not accept a popup without a
	 * parent xdg_surface, and there is no way to invent one. */
	surf = wl_compositor_create_surface(comp);
	xdg_surf = xdg_wm_base_get_xdg_surface(wm_base, surf);
	xdg_surface_add_listener(xdg_surf, &xdg_impl, NULL);
	xdg_surface_get_toplevel(xdg_surf);
	parent_xdg = xdg_surf;
	wl_surface_commit(surf);
	wl_display_roundtrip(dpy);

	/* The popup. */
	surf = wl_compositor_create_surface(comp);
	xdg_surf = xdg_wm_base_get_xdg_surface(wm_base, surf);
	xdg_surface_add_listener(xdg_surf, &xdg_impl, NULL);

	pos = xdg_wm_base_create_positioner(wm_base);
	xdg_positioner_set_anchor_rect(pos, 20, 20, 40, 40);
	xdg_positioner_set_size(pos, width, height);
	xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
	xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_LEFT);

	popup = xdg_surface_get_popup(xdg_surf, parent_xdg, pos);
	if (!popup) {
		fprintf(stderr, "lindpop: get_popup вернул NULL\n");
		return 1;
	}
	xdg_popup_add_listener(popup, &popup_impl, NULL);
	printf("lindpop: popup создан, %dx%d\n", width, height);

	stride = width * 4;
	size = stride * height;
	px = alloc_pixels(size, &fd);
	if (!px) {
		fprintf(stderr, "lindpop: не выделить буфер\n");
		return 1;
	}
	for (int i = 0; i < width * height; i++)
		px[i] = 0xFF1E88E5;

	pool = wl_shm_create_pool(shm, fd, size);
	buf = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
					WL_SHM_FORMAT_ARGB8888);
	if (!buf) {
		fprintf(stderr, "lindpop: буфер не создался\n");
		return 1;
	}

	wl_surface_attach(surf, buf, 0, 0);
	wl_surface_damage(surf, 0, 0, width, height);
	wl_surface_commit(surf);

	/* Block until configure. Until it arrives the client may not draw. */
	for (int i = 0; i < 50 && !configured; i++)
		wl_display_roundtrip(dpy);

	if (!configured) {
		fprintf(stderr, "lindpop: configure не пришёл\n");
		return 1;
	}
	printf("lindpop: configure получен, %dx%d в (%d, %d) панели\n",
	       width, height, cfg_x, cfg_y);

	/* Now draw, and stay up so a tap outside can dismiss it. */
	for (int i = 0; i < width * height; i++)
		px[i] = 0xFF1E88E5;
	for (int y = 0; y < height && y < 6; y++)
		for (int x = 0; x < width; x++)
			px[y * width + x] = 0xFF0D47A1;
	wl_surface_attach(surf, buf, 0, 0);
	wl_surface_damage(surf, 0, 0, width, height);
	wl_surface_commit(surf);
	wl_display_roundtrip(dpy);

	end = time(NULL) + secs;
	while (!dismissed && time(NULL) < end)
		wl_display_roundtrip(dpy);

	printf("lindpop: %d с на экране%s\n", secs,
	       dismissed ? ", закрыт тапом мимо" : ", вышел по таймеру");
	return 0;
}