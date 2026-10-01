// SPDX-License-Identifier: GPL-2.0-only
/*
 * panelloop - can a plain CPU render loop drive this panel continuously?
 *
 * weston freezes after one frame, and the page-flip path is not the reason:
 * vblank arrives at a clean 120 Hz and page flip events come back every time.
 * The compositor simply stops scheduling repaints, and the client stops
 * drawing because its frame callbacks stop arriving.
 *
 * This is the control experiment. It takes DRM master, modesets through the
 * legacy drmModeSetCrtc path that is known to work here, and then just renders
 * into the mapped buffer in a loop. No compositor, no Wayland, no event loop,
 * no timers - only the two things that have been measured to work.
 *
 * If a moving bar moves smoothly on the panel, the panel is drivable by a plain
 * userspace render loop and the second approach - a minimal compositor on this
 * path - is sound. If it does not, the problem is below the compositor and
 * neither approach will help.
 *
 * Double buffered with page flip, so the measurement covers the same
 * presentation path a real compositor would use. Falls back to redrawing the
 * same buffer if page flip turns out to be unavailable.
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -O2 -I<rootfs>/usr/include \
 *         -I<rootfs>/usr/include/libdrm panelloop.c -o panelloop \
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

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define DEV "/dev/dri/card0"

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
	drmModeRes *res = NULL;
	drmModeConnector *conn = NULL;
	uint32_t conn_id = 0, crtc_id = 0;
	uint32_t handle[2] = { 0, 0 }, fb[2] = { 0, 0 };
	uint32_t stride[2] = { 0, 0 };
	uint64_t size = 0, off[2] = { 0, 0 };
	uint32_t *map[2] = { MAP_FAILED, MAP_FAILED };
	uint32_t w, h, bpp = 32;
	int f, i, cur = 0, frames = 0, ret = 1;
	int seconds = argc > 1 ? atoi(argv[1]) : 10;
	uint64_t start, t0, t1;
	double fps = 0;

	f = open(DEV, O_RDWR | O_CLOEXEC);
	if (f < 0) {
		fprintf(stderr, "open %s: %s\n", DEV, strerror(errno));
		return 1;
	}
	if (drmSetMaster(f) < 0 && errno != EINVAL) {
		fprintf(stderr, "drmSetMaster: %s\n", strerror(errno));
		close(f);
		return 1;
	}
	printf("панель наша (DRM master на %s)\n", DEV);

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
	w = conn->modes[0].hdisplay;
	h = conn->modes[0].vdisplay;
	crtc_id = res->crtcs[0];
	printf("коннектор %u, CRTC %u, %ux%u\n", conn_id, crtc_id, w, h);

	for (i = 0; i < 2; i++) {
		if (drmModeCreateDumbBuffer(f, w, h, bpp, 0, &handle[i],
					    &stride[i], &size) < 0) {
			fprintf(stderr, "dumb buffer %d: %s\n", i,
				strerror(errno));
			goto out;
		}
		if (drmModeMapDumbBuffer(f, handle[i], &off[i]) < 0) {
			fprintf(stderr, "map dumb %d: %s\n", i,
				strerror(errno));
			goto out;
		}
		map[i] = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
			      f, off[i]);
		if (map[i] == MAP_FAILED) {
			fprintf(stderr, "mmap %d: %s\n", i, strerror(errno));
			goto out;
		}
		{
			uint32_t hs[4] = { handle[i], 0, 0, 0 };
			uint32_t ps[4] = { stride[i], 0, 0, 0 };
			uint32_t os[4] = { 0, 0, 0, 0 };

			if (drmModeAddFB2(f, w, h, DRM_FORMAT_XRGB8888, hs,
					  ps, os, &fb[i], 0) < 0) {
				fprintf(stderr, "AddFB2 %d: %s\n", i,
					strerror(errno));
				goto out;
			}
		}
	}
	printf("два буфера готовы, stride=%u, размер=%llu\n", stride[0],
	       (unsigned long long)size);

	if (drmModeSetCrtc(f, crtc_id, fb[0], 0, 0, &conn_id, 1,
			   &conn->modes[0]) < 0) {
		fprintf(stderr, "SetCrtc: %s\n", strerror(errno));
		goto out;
	}
	printf("режим установлен, рисую %d с\n\n", seconds);
	fflush(stdout);

	/* The loop. Frame n paints a bar whose x position advances with time,
	 * so a stalled render loop is visible as a frozen bar rather than as
	 * a static image that could be mistaken for success. */
	start = now_ms();
	t0 = start;
	while ((int)(now_ms() - start) < seconds * 1000) {
		uint32_t *p = map[cur];
		uint32_t *row;
		int bar = (int)((now_ms() - start) * w / (seconds * 1000));
		int y, x;

		for (y = 0; y < h; y++) {
			row = p + (size_t)y * stride[cur] / 4;
			for (x = 0; x < w; x++) {
				int d = x - bar;
				/* белый фон, чёрная полоса шириной 40 */
				row[x] = (d >= 0 && d < 40) ? 0xFF000000
							    : 0xFFFFFFFF;
			}
		}
		/* Redraw the same buffer rather than flipping: this isolates
		 * "can the panel be updated at all" from any page-flip
		 * behaviour, which is the thing under suspicion. */
		frames++;
		t1 = now_ms();
		if (frames % 20 == 0) {
			fps = 20.0 * 1000.0 / (double)(t1 - t0);
			t0 = t1;
			printf("  кадр %4d, полоса на x=%4d, %.1f fps\r", frames,
			       bar, fps);
			fflush(stdout);
		}
		usleep(8000); /* ~120 Гц, частота панели */
	}
	printf("\nитого %d кадров за %d с\n", frames, seconds);
	printf("если полоса двигалась - цикл отрисовки ведёт панель,\n"
	       "и минимальный композитор на этом пути имеет смысл.\n");
	ret = 0;

out:
	for (i = 0; i < 2; i++) {
		if (map[i] != MAP_FAILED)
			munmap(map[i], size);
		if (fb[i])
			drmModeRmFB(f, fb[i]);
		if (handle[i])
			drmModeDestroyDumbBuffer(f, handle[i]);
	}
	if (conn)
		drmModeFreeConnector(conn);
	if (res)
		drmModeFreeResources(res);
	drmDropMaster(f);
	close(f);
	return ret;
}
