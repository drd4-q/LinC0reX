// SPDX-License-Identifier: GPL-2.0-only
/*
 * kmsclaim - take the physical panel from Android and scan out a test image.
 *
 * This answers one question with no compositor in the way: can a plain Linux
 * process, from a chroot, become DRM master on card0 and program the panel?
 * If yes, the "Linux owns the screen" plan works, and everything after that
 * (compositor, phosh, GPU) is detail on top of it.
 *
 * weston could not answer it because its DRM backend resolves the device
 * through libdrm's device list instead of the path it is handed, and inside
 * the chroot that list is malformed.  Opening /dev/dri/card0 directly is
 * known to work there.
 *
 * Both commit paths are tried: legacy drmModeSetCrtc first, then atomic.
 *
 * Debian's libdrm 2.4.114 predates drmModeGetObjectProperties(), and this
 * CAF kernel's UAPI still calls that ioctl DRM_IOCTL_MODE_OBJ_GETPROPERTIES
 * (0xB9) rather than the upstream GETOBJECTPROPERTIES, so the property lookups
 * go through the raw ioctl.  The kernel and the libdrm headers agree on the
 * number, which is what makes that safe.
 *
 * Rendering is CPU-only via a dumb buffer, so nothing here says anything
 * about GPU support.
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -O2 -I<rootfs>/usr/include \
 *         -I<rootfs>/usr/include/libdrm kmsclaim.c -o kmsclaim \
 *         -L<rootfs>/usr/lib/aarch64-linux-gnu -ldrm
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define DEV "/dev/dri/card0"

static int fd;

/* Colour bars, so it is obvious the panel is showing our buffer. */
static void paint(uint32_t *px, int w, int h, int stride_px)
{
	static const uint32_t bar[8] = {
		0xFFFFFFFF, 0xFFFF00FF, 0x00FF00FF, 0x00FFFFFF,
		0xFF0000FF, 0xFFFF00FF, 0x0000FFFF, 0xFF00FFFF,
	};
	int x, y;

	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			px[y * stride_px + x] = bar[(x * 8) / w];
}

/*
 * Read every property that applies to one object.  Returns a malloc'd array
 * of count entries, each { prop_id, value }, or NULL on failure.
 */
static uint64_t *obj_props(uint32_t obj_id, uint32_t obj_type, uint32_t *count)
{
	struct drm_mode_obj_get_properties req;
	uint64_t *buf;
	uint32_t n, i;

	memset(&req, 0, sizeof(req));
	req.obj_id = obj_id;
	req.obj_type = obj_type;

	if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &req) < 0) {
		fprintf(stderr, "  OBJ_GETPROPERTIES(%u/%#x): %s\n", obj_id,
			obj_type, strerror(errno));
		return NULL;
	}

	n = req.count_props;
	if (!n) {
		*count = 0;
		return NULL;
	}

	buf = calloc(n, 2 * sizeof(*buf));
	if (!buf)
		return NULL;

	req.props_ptr = (uintptr_t)&buf[0];
	req.prop_values_ptr = (uintptr_t)&buf[n];

	if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &req) < 0) {
		fprintf(stderr, "  OBJ_GETPROPERTIES data: %s\n", strerror(errno));
		free(buf);
		return NULL;
	}

	/* libdrm hands back the two arrays interleaved as u64s: the first
	 * n entries are prop ids, the next n are values. */
	for (i = 0; i < n; i++) {
		uint32_t pid = (uint32_t)buf[i];

		buf[i * 2] = pid;
		buf[i * 2 + 1] = buf[n + i];
	}

	*count = n;
	return buf;
}

/* Look up a property id by name on one object, and optionally its value. */
static int find_prop(uint32_t obj_id, uint32_t obj_type, const char *name,
		     uint32_t *out_id, uint64_t *out_val)
{
	uint32_t count = 0, i;
	uint64_t *p = obj_props(obj_id, obj_type, &count);
	int found = -1;

	if (!p)
		return -1;

	for (i = 0; i < count; i++) {
		drmModePropertyPtr pr = drmModeGetProperty(fd, p[i * 2]);

		if (!pr)
			continue;
		if (!strcmp(pr->name, name)) {
			if (out_id)
				*out_id = p[i * 2];
			if (out_val)
				*out_val = p[i * 2 + 1];
			found = 0;
		}
		drmModeFreeProperty(pr);
		if (!found)
			break;
	}

	free(p);
	return found;
}

int main(void)
{
	drmModeRes *res = NULL;
	drmModeConnector *conn = NULL;
	drmModePlaneRes *planes = NULL;
	uint32_t conn_id = 0, crtc_id = 0, plane_id = 0;
	uint32_t fb_id = 0, handle = 0, stride = 0, mode_blob = 0;
	uint64_t size = 0;
	uint32_t *map = MAP_FAILED;
	uint32_t w, h;
	int i, ret = 1, committed = 0;
	drmModeAtomicReqPtr req = NULL;

	fd = open(DEV, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", DEV, strerror(errno));
		return 1;
	}
	printf("открыл %s (fd=%d)\n", DEV, fd);

	if (drmSetMaster(fd) < 0 && errno != EINVAL) {
		fprintf(stderr, "drmSetMaster: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	printf("стал DRM master\n");

	res = drmModeGetResources(fd);
	if (!res || res->count_crtcs == 0 || res->count_connectors == 0) {
		fprintf(stderr, "нет CRTC/коннекторов\n");
		goto out;
	}
	printf("CRTC=%d коннекторов=%d кодировщиков=%d\n",
	       res->count_crtcs, res->count_connectors, res->count_encoders);

	for (i = 0; i < res->count_connectors; i++) {
		conn = drmModeGetConnector(fd, res->connectors[i]);
		if (!conn)
			continue;
		printf("  коннектор %u: %s, режимов %d\n", conn->connector_id,
		       conn->connection == DRM_MODE_CONNECTED ? "подключён" :
		       conn->connection == DRM_MODE_DISCONNECTED ? "отключён" :
								  "неизвестно",
		       conn->count_modes);
		if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes)
			break;
		drmModeFreeConnector(conn);
		conn = NULL;
	}
	if (!conn) {
		fprintf(stderr, "нет подключённого коннектора с режимом\n");
		goto out;
	}
	conn_id = conn->connector_id;
	w = conn->modes[0].hdisplay;
	h = conn->modes[0].vdisplay;
	printf("  выбран коннектор %u: %ux%u\n", conn_id, w, h);

	/* This libdrm's drmModeCrtc has no connector field, so ask which
	 * CRTCs the connector can be driven by and take the first index. */
	{
		uint32_t poss = drmModeConnectorGetPossibleCrtcs(fd, conn);

		printf("  возможные CRTC: 0x%x\n", poss);
		if (!poss) {
			fprintf(stderr, "коннектор не привязан ни к одному CRTC\n");
			goto out;
		}
		for (i = 0; i < res->count_crtcs; i++) {
			if (poss & (1u << i)) {
				crtc_id = res->crtcs[i];
				break;
			}
		}
		if (!crtc_id) {
			fprintf(stderr, "индекс CRTC вне диапазона\n");
			goto out;
		}
	}
	{
		drmModeCrtc *c = drmModeGetCrtc(fd, crtc_id);

		if (c) {
			printf("  CRTC %u: текущий fb=%u, %dx%d%s\n",
			       crtc_id, c->buffer_id, c->width, c->height,
			       c->mode_valid ? "" : " (режим невалиден)");
			drmModeFreeCrtc(c);
		}
	}

	/* Primary plane, needed by the atomic path.  drmModePlane here has no
	 * type field, so read the "type" property off the object: 1 = Primary. */
	planes = drmModeGetPlaneResources(fd);
	if (planes) {
		printf("  плоскостей: %u\n", planes->count_planes);
		for (i = 0; i < (int)planes->count_planes; i++) {
			drmModePlane *p = drmModeGetPlane(fd, planes->planes[i]);
			uint64_t type = ~0ULL;

			if (!p)
				continue;
			if (find_prop(p->plane_id, DRM_MODE_OBJECT_PLANE, "type",
				      NULL, &type) == 0 && type == 1 &&
			    (p->possible_crtcs & (1u << i))) {
				plane_id = p->plane_id;
				printf("  плоскость %u: primary, crtc=%u fb=%u "
				       "possible_crtcs=0x%x\n",
				       plane_id, p->crtc_id, p->fb_id,
				       p->possible_crtcs);
			}
			drmModeFreePlane(p);
			if (plane_id)
				break;
		}
	}

	/* Dumb buffer: plain CPU memory, no GPU involved. */
	if (drmModeCreateDumbBuffer(fd, w, h, 32, 0, &handle, &stride,
				    &size) < 0) {
		fprintf(stderr, "drmModeCreateDumbBuffer: %s\n", strerror(errno));
		goto out;
	}
	printf("  dumb buffer %ux%u stride=%u size=%llu\n", w, h, stride,
	       (unsigned long long)size);

	{
		uint64_t off = 0;

		if (drmModeMapDumbBuffer(fd, handle, &off) < 0) {
			fprintf(stderr, "drmModeMapDumbBuffer: %s\n",
				strerror(errno));
			goto out;
		}
		printf("  dumb buffer offset=%llu\n", (unsigned long long)off);

		map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
			   off);
		if (map == MAP_FAILED) {
			fprintf(stderr, "mmap: %s\n", strerror(errno));
			goto out;
		}
	}
	paint(map, w, h, stride / 4);

	{
		uint32_t handles[4] = { handle, 0, 0, 0 };
		uint32_t pitches[4] = { stride, 0, 0, 0 };
		uint32_t offsets[4] = { 0, 0, 0, 0 };

	if (drmModeAddFB2(fd, w, h, DRM_FORMAT_XRGB8888, handles, pitches,
			  offsets, &fb_id, 0) < 0) {
		fprintf(stderr, "drmModeAddFB2: %s\n", strerror(errno));
		goto out;
	}
	}
	printf("  framebuffer %u\n", fb_id);

	/* --- attempt 1: legacy --- */
	printf("\n== legacy drmModeSetCrtc ==\n");
	if (drmModeSetCrtc(fd, crtc_id, fb_id, 0, 0, &conn_id, 1,
			   &conn->modes[0]) == 0) {
		printf("*** LEGACY MODESET OK ***\n");
		committed = 1;
	} else {
		printf("  не сработал: %s\n", strerror(errno));
	}

	/* --- attempt 2: atomic --- */
	if (!committed) {
		uint32_t blob = 0;
		uint32_t p_mode = 0, p_active = 0, p_conn_crtc = 0;
		uint32_t p_fb = 0, p_srcw = 0, p_srch = 0, p_crtcw = 0, p_crtch = 0;
		uint32_t p_pl_crtc = 0;

		printf("\n== atomic commit ==\n");
		if (drmModeCreatePropertyBlob(fd, &conn->modes[0],
					      sizeof(conn->modes[0]),
					      &blob) < 0) {
			fprintf(stderr, "  create blob: %s\n", strerror(errno));
			goto out;
		}
		mode_blob = blob;

		find_prop(crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", &p_mode, NULL);
		find_prop(crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", &p_active, NULL);
		find_prop(conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID",
			  &p_conn_crtc, NULL);
		if (plane_id) {
			find_prop(plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID",
				  &p_fb, NULL);
			find_prop(plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID",
				  &p_pl_crtc, NULL);
			find_prop(plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W",
				  &p_srcw, NULL);
			find_prop(plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H",
				  &p_srch, NULL);
			find_prop(plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W",
				  &p_crtcw, NULL);
			find_prop(plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H",
				  &p_crtch, NULL);
		}
		printf("  props: MODE_ID=%u ACTIVE=%u CONN_CRTC=%u "
		       "FB_ID=%u SRC=%ux%u CRTC=%ux%u\n",
		       p_mode, p_active, p_conn_crtc, p_fb, p_srcw, p_srch,
		       p_crtcw, p_crtch);

		req = drmModeAtomicAlloc();
		if (!req) {
			fprintf(stderr, "  атомарный режим не поддержан\n");
			goto out;
		}
		if (p_mode)
			drmModeAtomicAddProperty(req, crtc_id,
						p_mode, blob);
		if (p_active)
			drmModeAtomicAddProperty(req, crtc_id,
						p_active, 1);
		if (p_conn_crtc)
			drmModeAtomicAddProperty(req, conn_id,
						p_conn_crtc, crtc_id);
		if (plane_id) {
			if (p_fb)
				drmModeAtomicAddProperty(req, plane_id,
						p_fb, fb_id);
			if (p_pl_crtc)
				drmModeAtomicAddProperty(req, plane_id,
						p_pl_crtc, crtc_id);
			if (p_srcw)
				drmModeAtomicAddProperty(req, plane_id,
						p_srcw, w << 16);
			if (p_srch)
				drmModeAtomicAddProperty(req, plane_id,
						p_srch, h << 16);
			if (p_crtcw)
				drmModeAtomicAddProperty(req, plane_id,
						p_crtcw, w);
			if (p_crtch)
				drmModeAtomicAddProperty(req, plane_id,
						p_crtch, h);
		}

		if (drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET,
					NULL) == 0) {
			printf("*** ATOMIC MODESET OK ***\n");
			committed = 1;
		} else {
			printf("  не сработал: %s\n", strerror(errno));
		}
		drmModeAtomicFree(req);
		req = NULL;
	}

	if (committed) {
		printf("\nПАНЕЛЬ ЗАПРОГРАММИРОВАНА.\n"
		       "Держим её: Ctrl-C или kill вернёт экран Android.\n");
		fflush(stdout);
		for (;;)
			pause();
	}

	printf("\nпанель не захвачена\n");
	ret = 1;

out:
	if (map != MAP_FAILED)
		munmap(map, size);
	if (fb_id)
		drmModeRmFB(fd, fb_id);
	if (handle)
		drmModeDestroyDumbBuffer(fd, handle);
	if (mode_blob)
		drmModeDestroyPropertyBlob(fd, mode_blob);
	if (planes)
		drmModeFreePlaneResources(planes);
	if (conn)
		drmModeFreeConnector(conn);
	if (res)
		drmModeFreeResources(res);
	drmDropMaster(fd);
	close(fd);
	return ret;
}
