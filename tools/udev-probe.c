// SPDX-License-Identifier: GPL-2.0-only
/*
 * udev-probe - is libudev able to enumerate DRM nodes in this chroot?
 *
 * Android has no udev, so the database under /run/udev/data is hand-built.
 * libdrm resolves device names through libudev, and if that comes back empty
 * both modetest and weston report "no device found" even though /dev/dri is
 * right there.  This prints what each layer actually sees.
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -O2 -ldl -o udev-probe udev-probe.c
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
	void *lib;
	struct udev *(*u_new)(void);
	struct udev_enumerate *(*e_new)(void);
	int (*e_add)(struct udev_enumerate *, const char *);
	int (*e_scan)(struct udev_enumerate *);
	struct udev_list_entry *(*e_list)(struct udev_enumerate *);
	struct udev_list_entry *(*le_next)(struct udev_list_entry *);
	const char *(*le_name)(struct udev_list_entry *);
	struct udev_device *(*d_new)(struct udev *, const char *);
	const char *(*d_devnode)(struct udev_device *);
	const char *(*d_prop)(struct udev_device *, const char *);
	void (*d_unref)(struct udev_device *);
	void (*e_unref)(struct udev_enumerate *);
	void (*u_unref)(struct udev *);
	struct udev *u;
	struct udev_enumerate *e;
	struct udev_list_entry *le;
	int n = 0;

	lib = dlopen("libudev.so.1", RTLD_NOW);
	if (!lib) {
		printf("dlopen libudev.so.1: %s\n", dlerror());
		return 1;
	}
	printf("libudev: загружена\n");

#define S(sym, real) do { *(void **)(&sym) = dlsym(lib, real); \
	if (!sym) { printf("нет символа %s\n", real); return 1; } } while (0)
	S(u_new, "udev_new"); S(e_new, "udev_enumerate_new"); S(e_add, "udev_enumerate_add_match_subsystem"); S(e_scan, "udev_enumerate_scan_devices"); S(e_list, "udev_enumerate_get_list_entry"); S(le_next, "udev_list_entry_get_next");
	S(le_name, "udev_list_entry_get_name"); S(d_new, "udev_device_new_from_syspath"); S(d_devnode, "udev_device_get_devnode"); S(d_prop, "udev_device_get_property_value"); S(d_unref, "udev_device_unref");
	S(e_unref, "udev_enumerate_unref"); S(u_unref, "udev_unref");
#undef S

	u = u_new();
	e = e_new();
	e_add(e, "drm");
	e_scan(e);

	le = e_list(e);
	while (le) {
		const char *name = le_name(le);
		struct udev_device *d;

		le = le_next(le);
		if (!name)
			continue;

		d = d_new(u, name);
		if (!d)
			continue;

		printf("  syspath=%s\n", name);
		printf("    DEVNAME = %s\n", d_devnode(d) ? d_devnode(d) : "(null)");
		printf("    MAJOR   = %s\n", d_prop(d, "MAJOR") ?: "(null)");
		printf("    MINOR   = %s\n", d_prop(d, "MINOR") ?: "(null)");
		printf("    SUBSYS  = %s\n", d_prop(d, "SUBSYSTEM") ?: "(null)");
		d_unref(d);
		n++;
	}
	printf("всего drm-устройств: %d\n", n);

	e_unref(e);
	u_unref(u);
	return 0;
}
