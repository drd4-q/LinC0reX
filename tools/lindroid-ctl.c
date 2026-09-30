// SPDX-License-Identifier: GPL-2.0-only
/*
 * lindroid-ctl - drive the Lindroid EVDI display from a plain Linux userspace
 *
 * Copyright (c) 2025 Lindroid Authors
 *
 * Talks to the display-control ioctls added by the evdi-lindroid driver
 * (see uapi/evdi_drm.h).  Typical use is handing the physical panel over to
 * a Wayland compositor such as Weston running Phosh or GNOME Mobile, and
 * taking it back afterwards.
 *
 *   lindroid-ctl status
 *   lindroid-ctl to-linux        # hand the panel to Linux
 *   lindroid-ctl to-android      # give the panel back to Android
 *   lindroid-ctl screen off|on   # blank / light the display
 *   lindroid-ctl wait <owner>    # block until the owner becomes <owner>
 *
 * Build:  make -C drivers/lindroid-drm/tools
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <drm/drm.h>

#include "evdi_drm.h"

#define EVDI_DRIVER_NAME "evdi-lindroid"

static int verbose;

static int evdi_control(int fd, __s32 cmd, __s32 display_id, __s32 arg,
			__s32 *out)
{
	struct drm_evdi_control ctl;

	memset(&ctl, 0, sizeof(ctl));
	ctl.cmd = cmd;
	ctl.display_id = display_id;
	ctl.arg = arg;

	if (ioctl(fd, DRM_IOCTL_EVDI_CONTROL, &ctl) < 0) {
		if (verbose)
			fprintf(stderr, "CONTROL cmd=%d failed: %s\n",
				cmd, strerror(errno));
		return -1;
	}

	if (out)
		*out = ctl.ret;

	return 0;
}

static int evdi_get_info(int fd, __s32 display_id,
			 struct drm_evdi_display_info *info)
{
	memset(info, 0, sizeof(*info));
	info->display_id = display_id;

	return ioctl(fd, DRM_IOCTL_EVDI_GET_INFO, info) < 0 ? -1 : 0;
}

static const char *owner_str(__s32 owner)
{
	return owner == EVDI_OWNER_LINUX ? "linux" : "android";
}

/* Find /dev/dri/cardX whose backing driver is evdi-lindroid. */
static int open_evdi_card(void)
{
	DIR *dir;
	struct dirent *ent;
	int fd = -1;

	dir = opendir("/sys/class/drm");
	if (!dir)
		return -1;

	while ((ent = readdir(dir))) {
		char path[PATH_MAX], value[256];
		const char *driver;
		char node[64];
		int len;

		if (strncmp(ent->d_name, "card", 4))
			continue;
		if (!strspn(ent->d_name + 4, "0123456789"))
			continue;
		if (ent->d_name[4])
			continue;

		snprintf(path, sizeof(path), "/sys/class/drm/%s/device/uevent",
			 ent->d_name);

		driver = NULL;
		FILE *f = fopen(path, "r");
		if (f) {
			while (fgets(value, sizeof(value), f)) {
				if (!strncmp(value, "DRIVER=", 7)) {
					driver = strchr(value, '=');
					if (driver)
						driver++;
					break;
				}
			}
			fclose(f);
		}

		if (!driver || strncmp(driver, EVDI_DRIVER_NAME,
				       strlen(EVDI_DRIVER_NAME)))
			continue;

		len = snprintf(node, sizeof(node), "/dev/dri/%s", ent->d_name);
		if (len < 0 || (size_t)len >= sizeof(node))
			continue;

		fd = open(node, O_RDWR | O_CLOEXEC);
		if (fd >= 0) {
			if (verbose)
				fprintf(stderr, "using %s\n", node);
			break;
		}
	}

	closedir(dir);

	return fd;
}

static int cmd_status(int fd)
{
	struct drm_evdi_display_info info;
	__s32 ndisplays = 0;
	int i, ret = 0;

	if (evdi_control(fd, EVDI_CTL_GET_NDISPLAYS, 0, 0, &ndisplays) ||
	    ndisplays <= 0) {
		fprintf(stderr, "driver did not report a display count\n");
		return 1;
	}

	for (i = 0; i < ndisplays; i++) {
		if (evdi_get_info(fd, i, &info) < 0)
			return 1;

		if (!info.connected && !info.clients)
			continue;

		printf("display%d: %ux%u@%uHz owner=%s acked=%s screen=%s "
		       "power=%d clients=%d gen=%u\n",
		       info.display_id, info.width, info.height,
		       info.refresh_rate, owner_str(info.owner),
		       owner_str(info.acked_owner),
		       info.screen == EVDI_SCREEN_ON ? "on" : "off",
		       info.power_mode, info.clients, info.generation);
		ret = 0;
	}

	return ret;
}

static int cmd_wait(int fd, __s32 target)
{
	struct drm_evdi_control ctl;
	__s32 owner = target;

	memset(&ctl, 0, sizeof(ctl));
	ctl.cmd = EVDI_CTL_GET_OWNER;
	ctl.arg = target;

	/*
	 * Generation 0 means "wait for the next change", which is what a
	 * caller that just changed something wants.
	 */
	ctl.generation = 0;

	while (ioctl(fd, DRM_IOCTL_EVDI_WAIT_EVENT, &ctl) < 0) {
		if (errno == EINTR)
			continue;
		if (errno == ETIMEDOUT) {
			fprintf(stderr, "timeout waiting for owner=%s\n",
				owner_str(target));
			return 1;
		}
		perror("EVDI_WAIT_EVENT");
		return 1;
	}

	owner = ctl.ret;
	printf("owner is now %s (generation %llu)\n", owner_str(owner),
	       (unsigned long long)ctl.generation);

	return owner == target ? 0 : 1;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [-v] [-c card] <command>\n"
		"\n"
		"commands:\n"
		"  status              show display state\n"
		"  to-linux            hand the panel to the Linux compositor\n"
		"  to-android          give the panel back to Android\n"
		"  screen on|off       light or blank the display\n"
		"  wait android|linux  block until the owner changes\n",
		argv0);
}

int main(int argc, char **argv)
{
	const char *cmd;
	const char *node = NULL;
	int fd;
	int i = 1;
	int ret = 1;

	if (i < argc && !strcmp(argv[i], "-v")) {
		verbose = 1;
		i++;
	}

	if (i < argc && !strcmp(argv[i], "-c")) {
		if (i + 1 >= argc) {
			usage(argv[0]);
			return 2;
		}
		node = argv[i + 1];
		i += 2;
	}

	if (i >= argc) {
		usage(argv[0]);
		return 2;
	}

	cmd = argv[i++];

	if (node) {
		fd = open(node, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			fprintf(stderr, "%s: %s\n", node, strerror(errno));
			return 1;
		}
	} else {
		fd = open_evdi_card();
		if (fd < 0) {
			fprintf(stderr,
				"no %s DRM card found, is the Lindroid kernel loaded?\n",
				EVDI_DRIVER_NAME);
			return 1;
		}
	}

	if (!strcmp(cmd, "status")) {
		ret = cmd_status(fd);
	} else if (!strcmp(cmd, "to-linux")) {
		ret = evdi_control(fd, EVDI_CTL_SET_OWNER, 0,
				   EVDI_OWNER_LINUX, NULL) ? 1 : 0;
	} else if (!strcmp(cmd, "to-android")) {
		ret = evdi_control(fd, EVDI_CTL_SET_OWNER, 0,
				   EVDI_OWNER_ANDROID, NULL) ? 1 : 0;
	} else if (!strcmp(cmd, "screen")) {
		__s32 state;

		if (i >= argc) {
			usage(argv[0]);
			goto out;
		}

		if (!strcmp(argv[i], "on"))
			state = EVDI_SCREEN_ON;
		else if (!strcmp(argv[i], "off"))
			state = EVDI_SCREEN_OFF;
		else {
			fprintf(stderr, "screen: expected on|off\n");
			goto out;
		}

		ret = evdi_control(fd, EVDI_CTL_SET_SCREEN, 0, state,
				   NULL) ? 1 : 0;
	} else if (!strcmp(cmd, "wait")) {
		__s32 target;

		if (i >= argc) {
			usage(argv[0]);
			goto out;
		}

		if (!strcmp(argv[i], "linux"))
			target = EVDI_OWNER_LINUX;
		else if (!strcmp(argv[i], "android"))
			target = EVDI_OWNER_ANDROID;
		else {
			fprintf(stderr, "wait: expected android|linux\n");
			goto out;
		}

		ret = cmd_wait(fd, target);
	} else {
		usage(argv[0]);
		ret = 2;
	}

out:
	close(fd);

	return ret;
}
