// SPDX-License-Identifier: GPL-2.0-only
/*
 * drmprobe - reproduce what weston's "direct launcher" actually does.
 *
 * weston's drm_launcher_v1_direct() calls drmGetDevices2() and then open()s
 * the node that comes back.  On this device udev enumeration works and
 * drmGetDevices2 returns a device, yet weston reports
 * "ERROR: could not open DRM device" followed by "no drm device found" —
 * so the failing step is the open.  This prints the node path and the errno.
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -O2 -I<rootfs>/usr/include \
 *         -L<rootfs>/usr/lib/aarch64-linux-gnu -ldrm drmprobe.c -o drmprobe
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include <xf86drm.h>

int main(void)
{
	drmDevicePtr devs[8];
	int count, i, n;

	count = drmGetDevices2(0, devs, 8);
	printf("drmGetDevices2: %d устройств(а)\n", count);

	for (i = 0; i < count; i++) {
		printf("устройство %d: bustype=%d available_nodes=%d\n",
		       i, devs[i]->bustype, devs[i]->available_nodes);

		for (n = 0; n < DRM_NODE_MAX; n++) {
			const char *name = devs[i]->nodes[n];
			int fd;

			if (!name)
				continue;

			printf("  node[%d] = %s -> ", n, name);
			fflush(stdout);

			fd = open(name, O_RDWR | O_CLOEXEC);
			if (fd < 0) {
				printf("open FAILED errno=%d (%s)\n", errno,
				       strerror(errno));
			} else {
				printf("open OK fd=%d\n", fd);
				close(fd);
			}
		}
	}

	return count > 0 ? 0 : 1;
}
