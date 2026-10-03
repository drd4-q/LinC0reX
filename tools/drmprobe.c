/*
 * drmprobe - does this phone's render node offer anything to a GPU client?
 *
 * Written to settle one question that decides the whole direction of the
 * project: can Mesa, and therefore Phosh or any Qt Quick application, ever
 * work on this hardware? If yes, lind needs wl_drm and the software-rendering
 * dead end is temporary. If no, software rendering is permanent and the answer
 * is to write clients, not to port ones.
 *
 * The kernel here registers SDE as the only DRM driver. It is a KMS driver:
 * it hands out framebuffers and scanout, which is everything a compositor
 * needs and nothing a 3D client needs. So the question is not "is the node
 * there" - it is "does it answer any of the calls a GL stack makes".
 *
 * Each step is reported separately, because "it fails" is useless and "it fails
 * at exactly this call" is the whole answer:
 *
 *   1  open                      - is the node reachable at all
 *   2  drmGetVersion2            - which driver claims it
 *   3  drmModeGetResources       - does it speak KMS
 *   4  create_dumb / map         - can userspace allocate memory it can map
 *   5  DRM_CAP_DUMB_BUFFER       - the capability bit Mesa's dumb backend wants
 *   6  gbm-style probe           - best-effort: a 32x32 dumb buffer
 *
 * Step 4/5 are where the answer lives. A KMS-only node can pass 1-3 and fail
 * 4-5, and that combination means Mesa has nothing to talk to.
 *
 * Usage: drmprobe [device]     default /dev/dri/renderD128
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

/*
 * The dumb-buffer ABI, declared here rather than included, because two of the
 * three header sets available disagree and picking the wrong one produces a
 * confident wrong answer.
 *
 * The libdrm in the chroot declares these shapes; the phone's kernel
 * (include/uapi/drm/drm_mode.h, Darkmoon-Reborn 5.4) declares them the old
 * way, with height before width and no pixel_format. The ioctl number is
 * _IOWR over the struct's size, so a struct that differs by even one __u32
 * becomes a different ioctl and the kernel answers EINVAL.
 *
 * That happened. A first version of this probe used the modern shape, got
 * EINVAL from create_dumb, and would have reported - as a hardware fact - that
 * the driver cannot allocate memory. It can; the ioctl number was wrong.
 *
 * These are the kernel's shapes, field for field.
 */
struct probe_create_dumb {
	uint32_t height;                /* note: before width, as the kernel has it */
	uint32_t width;
	uint32_t bpp;
	uint32_t flags;
	uint32_t handle;
	uint32_t pitch;
	uint64_t size;
};

struct probe_map_dumb {
	uint32_t handle;
	uint32_t pad;
	uint64_t offset;
};

#define PROBE_CREATE_DUMB \
	_IOWR('d', 0xb2, struct probe_create_dumb)
#define PROBE_MAP_DUMB \
	_IOWR('d', 0xb3, struct probe_map_dumb)

static int step_no;

static void step(const char *what, int ok, const char *detail)
{
	printf("%d. %-34s %s%s%s\n", ++step_no, what,
	       ok ? "ДА" : "НЕТ", detail && *detail ? " - " : "",
	       detail && *detail ? detail : "");
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/dri/renderD128";
	drmVersionPtr ver;
	struct _drmVersion *v;
	drmModeRes *res;
	int fd;
	uint64_t caps = 0;

	printf("drmprobe: %s\n\n", dev);

	/* 1 */
	fd = open(dev, O_RDWR | O_CLOEXEC);
	step("open", fd >= 0, fd < 0 ? strerror(errno) : NULL);
	if (fd < 0)
		return 1;

	/* 2 */
	ver = drmGetVersion(fd);
	step("drmGetVersion", ver != NULL, NULL);
	if (ver) {
		char d[256];

		snprintf(d, sizeof d, "%s %d.%d.%d", ver->name,
			 ver->version_major, ver->version_minor,
			 ver->version_patchlevel);
		printf("     драйвер: %s\n", d);
		step("  это KGSL/msm_kgsl", strstr(ver->name, "kgsl") != NULL,
		     strstr(ver->name, "kgsl") ? "" :
		     "не KGSL, значит GPU-слоя для него нет");
		v = (struct _drmVersion *)ver;
		drmFreeVersion(v);
	}

	/* 3 */
	res = drmModeGetResources(fd);
	step("drmModeGetResources (KMS)", res != NULL, NULL);
	if (res) {
		printf("     коннекторов %u, CRTC %u\n",
		       res->count_connectors, res->count_crtcs);
		drmModeFreeResources(res);
	}

	/* 4/5 - the part that matters */
	if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &caps) == 0)
		step("DRM_CAP_DUMB_BUFFER", caps != 0,
		     caps ? "Mesa сможет выделять через dumb" : "нет");
	else
		step("DRM_CAP_DUMB_BUFFER", 0, strerror(errno));

	/*
	 * 6 - actually try to allocate. The capability bit can be present and
	 * the ioctl still refuse, and only the refusal is proof.
	 */
	{
		struct probe_create_dumb fb;
		uint32_t handle = 0;
		struct probe_map_dumb map;
		int r;

		memset(&fb, 0, sizeof fb);
		fb.width = 32;
		fb.height = 32;
		fb.bpp = 32;

		r = drmIoctl(fd, PROBE_CREATE_DUMB, &fb);
		step("create_dumb 32x32", r == 0,
		     r == 0 ? "буфер выделен" : strerror(errno));
		handle = fb.handle;

		if (r == 0 && handle) {
			memset(&map, 0, sizeof map);
			map.handle = handle;

			r = drmIoctl(fd, PROBE_MAP_DUMB, &map);
			step("map_dumb", r == 0, r == 0 ? NULL : strerror(errno));
			if (r == 0) {
				void *p = mmap(0, fb.size,
					       PROT_READ | PROT_WRITE,
					       MAP_SHARED, fd, map.offset);
				step("mmap буфера", p != MAP_FAILED,
				     p == MAP_FAILED ? strerror(errno) : NULL);
				if (p != MAP_FAILED) {
					*(uint32_t *)p = 0xDEADBEEF;
					step("запись в буфер",
					     *(uint32_t *)p == 0xDEADBEEF, NULL);
					munmap(p, fb.size);
				}
			}
		}
	}

	close(fd);

	printf("\nитог: ");
	if (step_no >= 6)
		printf("узел даёт выделение памяти - GPU-путь возможен\n");
	else
		printf("узнать нельзя, см. выше\n");
	return 0;
}
