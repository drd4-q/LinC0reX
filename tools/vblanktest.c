// SPDX-License-Identifier: GPL-2.0-only
/*
 * vblanktest - do vblank and page-flip events actually arrive from SDE?
 *
 * weston starts its repaint loop with drmWaitVBlank(), and only falls back to
 * a page flip when that returns no usable timestamp. A first frame reaching
 * the panel followed by a frozen image is what you get if vblank events never
 * arrive and page-flip completion never comes back either: the loop issues one
 * frame, waits for an event that does not come, and stops.
 *
 * This measures that directly, with no compositor and no property lookups: take
 * DRM master, modeset the panel the way kmsclaim does, then ask for vblanks and
 * for a page flip, and report what came back.
 *
 * Deliberately minimal. A previous attempt folded this into kmsclaim, where a
 * heap corruption in the property-lookup path crashed it before the probe ran.
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -O2 -I<rootfs>/usr/include \
 *         -I<rootfs>/usr/include/libdrm vblanktest.c -o vblanktest \
 *         -L<rootfs>/usr/lib/aarch64-linux-gnu -ldrm
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <stdint.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

/* Not exposed by this libdrm's public headers; drmModePageFlip() takes a
 * pointer to it and the kernel writes the event back through the DRM fd.
 * Layout is from the kernel UAPI. */
struct drm_mode_page_flip_event {
	uint32_t type;
	uint32_t length;
	uint64_t sequence;
	uint64_t time;
	uint32_t sequence_nr;
	uint32_t reserved[4];
};

#define DEV "/dev/dri/card0"
#define TRIES 3

int main(void)
{
	drmModeRes *res = NULL;
	drmModeConnector *conn = NULL;
	uint32_t conn_id = 0, crtc_id = 0;
	uint32_t fb_id = 0, handle = 0, stride = 0, plane_id = 0;
	uint64_t size = 0, off = 0;
	int f, i, ret = 1;
	uint32_t *map = MAP_FAILED;
	struct drm_mode_page_flip_event *ev = NULL;

	f = open(DEV, O_RDWR | O_CLOEXEC);
	if (f < 0) {
		fprintf(stderr, "open %s: %s\n", DEV, strerror(errno));
		return 1;
	}
	printf("открыл %s\n", DEV);

	if (drmSetMaster(f) < 0 && errno != EINVAL) {
		fprintf(stderr, "drmSetMaster: %s\n", strerror(errno));
		close(f);
		return 1;
	}
	printf("стал DRM master\n");

	res = drmModeGetResources(f);
	if (!res) {
		fprintf(stderr, "drmModeGetResources не удался\n");
		goto out;
	}

	for (i = 0; i < res->count_connectors; i++) {
		conn = drmModeGetConnector(f, res->connectors[i]);
		if (!conn)
			continue;
		if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes)
			break;
		drmModeFreeConnector(conn);
		conn = NULL;
	}
	if (!conn) {
		fprintf(stderr, "нет подключённого коннектора\n");
		goto out;
	}
	conn_id = conn->connector_id;

	{
		uint32_t poss = drmModeConnectorGetPossibleCrtcs(f, conn);

		if (!poss) {
			fprintf(stderr, "коннектор не привязан к CRTC\n");
			goto out;
		}
		for (i = 0; i < res->count_crtcs; i++)
			if (poss & (1u << i)) {
				crtc_id = res->crtcs[i];
				break;
			}
	}
	printf("коннектор %u, CRTC %u, режим %ux%u\n", conn_id, crtc_id,
	       conn->modes[0].hdisplay, conn->modes[0].vdisplay);

	/* Any plane will do for a dumb buffer; scanout is what we are testing,
	 * not which plane the compositor would pick. */
	{
		drmModePlaneRes *pl = drmModeGetPlaneResources(f);

		if (pl && pl->count_planes)
			plane_id = pl->planes[0];
		if (pl)
			drmModeFreePlaneResources(pl);
	}

	if (drmModeCreateDumbBuffer(f, conn->modes[0].hdisplay,
				    conn->modes[0].vdisplay, 32, 0,
				    &handle, &stride, &size) < 0) {
		fprintf(stderr, "dumb buffer: %s\n", strerror(errno));
		goto out;
	}
	if (drmModeMapDumbBuffer(f, handle, &off) < 0) {
		fprintf(stderr, "map dumb: %s\n", strerror(errno));
		goto out;
	}
	map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, f, off);
	if (map == MAP_FAILED) {
		fprintf(stderr, "mmap: %s\n", strerror(errno));
		goto out;
	}
	/* Paint it, so a buffer is never mistaken for a blank one. */
	for (i = 0; i < (int)(size / 4); i++)
		map[i] = 0xFF00FFFF;

	{
		uint32_t handles[4] = { handle, 0, 0, 0 };
		uint32_t pitches[4] = { stride, 0, 0, 0 };
		uint32_t offsets[4] = { 0, 0, 0, 0 };

		if (drmModeAddFB2(f, conn->modes[0].hdisplay,
				  conn->modes[0].vdisplay, DRM_FORMAT_XRGB8888,
				  handles, pitches, offsets, &fb_id, 0) < 0) {
			fprintf(stderr, "AddFB2: %s\n", strerror(errno));
			goto out;
		}
	}
	printf("framebuffer %u готов\n", fb_id);

	if (drmModeSetCrtc(f, crtc_id, fb_id, 0, 0, &conn_id, 1,
			   &conn->modes[0]) < 0) {
		fprintf(stderr, "SetCrtc: %s\n", strerror(errno));
		goto out;
	}
	printf("режим установлен (legacy SetCrtc)\n\n");

	/* weston schedules the next repaint off the vblank timestamp it gets
	 * here, and it warns when that lands absurdly far from CLOCK_MONOTONIC
	 * ("computed repaint delay is insane", around -10 hours on this
	 * device). So compare the two directly: if DRM reports vblank in a
	 * different time base than weston assumes, the repaint loop is
	 * scheduled into the far past and never fires again. */
	{
		struct timespec mono;
		long uptime_s = 0;
		FILE *fup;

		clock_gettime(CLOCK_MONOTONIC, &mono);
		fup = fopen("/proc/uptime", "r");
		if (fup) {
			if (fscanf(fup, "%ld", &uptime_s) != 1)
				uptime_s = 0;
			fclose(fup);
		}
		printf("== часы ==\n");
		printf("  CLOCK_MONOTONIC = %ld с\n", (long)mono.tv_sec);
		printf("  /proc/uptime    = %ld с\n", uptime_s);
		printf("  unix time       = %ld с\n", (long)time(NULL));
	}

	/* --- vblank --- */
	printf("\n== drmWaitVBlank, %d попыток ==\n", TRIES);
	for (i = 0; i < TRIES; i++) {
		drmVBlank vbl = {
			.request.type = DRM_VBLANK_RELATIVE,
			.request.sequence = 1,
			.request.signal = 0,
		};

		errno = 0;
		if (drmWaitVBlank(f, &vbl) < 0) {
			printf("  #%d ошибка: %d (%s)\n", i, errno,
			       strerror(errno));
			continue;
		}
		printf("  #%d seq=%u  tval_sec=%ld  tval_usec=%lu\n", i,
		       vbl.reply.sequence, (long)vbl.reply.tval_sec,
		       (unsigned long)vbl.reply.tval_usec);
	}

	/* --- page flip --- */
	printf("\n== page flip, %d попыток ==\n", TRIES);
	printf("  (ждём событие через DRM fd, читаем с таймаутом)\n");
	ev = calloc(1, sizeof(*ev));
	if (!ev)
		goto out;

	for (i = 0; i < TRIES; i++) {
		int n;

		errno = 0;
		if (drmModePageFlip(f, crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT,
				    ev) < 0) {
			printf("  #%d page flip не удался: %d (%s)\n", i,
			       errno, strerror(errno));
			continue;
		}
		printf("  #%d отправлен, ждём событие...", i);
		fflush(stdout);

		/* Read exactly one event. -1 on EOF, and errno EAGAIN if the
		 * fd is non-blocking, which is the "nothing arrived" answer. */
		n = read(f, ev, sizeof(*ev));
		if (n < 0) {
			printf(" таймаут/ошибка %d (%s)\n", errno,
			       strerror(errno));
		} else if (n == 0) {
			printf(" EOF\n");
			break;
		} else {
			printf(" получено событие, seq=%lu\n",
			       (unsigned long)ev->sequence);
		}
		memset(ev, 0, sizeof(*ev));
	}

	printf("\nИтог: если drmWaitVBlank вернул usec=0 или ошибку, значит\n"
	       "события vblank не приходят. Если page flip не дал события,\n"
	       "то цикл перерисовки weston встанет после первого кадра.\n");

	ret = 0;

out:
	if (map != MAP_FAILED)
		munmap(map, size);
	free(ev);
	if (fb_id)
		drmModeRmFB(f, fb_id);
	if (handle)
		drmModeDestroyDumbBuffer(f, handle);
	if (conn)
		drmModeFreeConnector(conn);
	if (res)
		drmModeFreeResources(res);
	drmDropMaster(f);
	close(f);
	return ret;
}
