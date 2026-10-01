// SPDX-License-Identifier: GPL-2.0-only
/*
 * udev-probe - what does libudev actually see in this chroot?
 *
 * Android has no udev, so /run/udev/data is hand-built. Everything that
 * enumerates through libudev - libdrm, and libinput - comes back empty or
 * incomplete if those entries are wrong, and the resulting errors point
 * nowhere near the cause. This prints what each layer resolves, per subsystem.
 *
 * The is_initialized check is the one that matters for input: libinput skips
 * any event* node for which it is false (src/udev-seat.c), and weston then
 * dies with "no input devices" no matter how correct the rest looks.
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -O2 -ldl -o udev-probe udev-probe.c
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(int argc, char **argv)
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
	int (*d_init)(struct udev_device *);
	void (*d_unref)(struct udev_device *);
	void (*e_unref)(struct udev_enumerate *);
	void (*u_unref)(struct udev *);
	struct udev *u;
	struct udev_enumerate *e;
	struct udev_list_entry *le;
	const char *subsys = argc > 1 ? argv[1] : "drm";
	int n = 0, inited = 0;

	lib = dlopen("libudev.so.1", RTLD_NOW);
	if (!lib) {
		printf("dlopen libudev.so.1: %s\n", dlerror());
		return 1;
	}

#define S(sym, real) do { *(void **)(&sym) = dlsym(lib, real); \
	if (!sym) { printf("нет символа %s\n", real); return 1; } } while (0)
	S(u_new, "udev_new"); S(e_new, "udev_enumerate_new");
	S(e_add, "udev_enumerate_add_match_subsystem");
	S(e_scan, "udev_enumerate_scan_devices");
	S(e_list, "udev_enumerate_get_list_entry");
	S(le_next, "udev_list_entry_get_next");
	S(le_name, "udev_list_entry_get_name");
	S(d_new, "udev_device_new_from_syspath");
	S(d_devnode, "udev_device_get_devnode");
	S(d_prop, "udev_device_get_property_value");
	S(d_init, "udev_device_get_is_initialized");
	S(d_unref, "udev_device_unref");
	S(e_unref, "udev_enumerate_unref"); S(u_unref, "udev_unref");
#undef S

	printf("libudev загружена, подсистема \"%s\"\n", subsys);

	u = u_new();
	e = e_new();
	e_add(e, subsys);
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

		n++;
		printf("  %s\n", name);
		printf("    DEVNAME          = %s\n",
		       d_devnode(d) ? d_devnode(d) : "(null)");
		printf("    is_initialized   = %s\n",
		       d_init(d) ? "да" : "НЕТ");
		printf("    USEC_INITIALIZED = %s\n",
		       d_prop(d, "USEC_INITIALIZED") ?: "(null)");
		printf("    ID_INPUT         = %s\n",
		       d_prop(d, "ID_INPUT") ?: "(null)");
		printf("    ID_SEAT          = %s\n",
		       d_prop(d, "ID_SEAT") ?: "(null)");
		/* Not in any sysfs uevent file, so a non-null value here
		 * can only have come from the hand-built database. */
		printf("    ID_PATH          = %s\n",
		       d_prop(d, "ID_PATH") ?: "(null)");

		if (d_init(d))
			inited++;
		d_unref(d);
	}

	printf("итого: %d устройств, из них настроенных: %d\n", n, inited);

	e_unref(e);
	u_unref(u);
	return 0;
}
