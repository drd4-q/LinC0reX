// SPDX-License-Identifier: GPL-2.0-only
/*
 * fmtshim - make weston tolerate a DRM driver that lists a format twice.
 *
 * weston 10 aborts on this phone:
 *
 *   weston: ../libweston/drm-formats.c:131:
 *   weston_drm_format_array_add_format:
 *   Assertion `!weston_drm_format_array_find_format(formats, format)' failed.
 *
 * The assertion is correct as written - adding a format that is already in the
 * array is a caller error. The caller is the bug. In
 * libweston/backend-drm/kms.c, drm_plane_populate_formats() walks the plane's
 * IN_FORMATS modifier blob and calls weston_drm_format_array_add_format() once
 * per entry, with no check for a format it has already added. SDE advertises
 * the same fourcc twice, and weston dies.
 *
 * weston_drm_format_array_join(), a few lines below, does check. So the
 * invariant is real, just not enforced consistently.
 *
 * The duplicate is not cosmetic: the two entries carry different modifiers,
 * and dropping the second would lose one. So this does not skip the duplicate,
 * it looks the format up first and returns the existing entry, which is what
 * the caller meant. The second copy's modifiers are then merged in by the
 * caller's own loop, so nothing is lost.
 *
 * This intercepts rather than patches because libweston-10.so.0 is a Debian
 * binary package and the call crosses a shared-object boundary from
 * drm-backend.so, which makes it interposable. Patching would mean rebuilding
 * the library.
 *
 * Use:
 *   LD_PRELOAD=/path/fmtshim.so weston ...
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>

// Layouts copied from weston 10. These are stable within the 10.x ABI, which
// is the only thing this depends on.
struct wl_array {
	size_t size;
	size_t alloc;
	void *data;
};

struct weston_drm_format {
	uint32_t format;
	struct wl_array modifiers;
};

typedef struct weston_drm_format *(*add_format_fn)(struct wl_array *,
						    uint32_t);
typedef int (*add_mod_fn)(struct weston_drm_format *, uint64_t);

static add_format_fn real_add;
static add_mod_fn real_add_mod;

struct weston_drm_format *weston_drm_format_array_add_format(
		struct wl_array *formats, uint32_t format)
{
	struct weston_drm_format *fmts = formats->data;
	size_t n = formats->size / sizeof(*fmts);
	size_t i;

	if (!real_add)
		real_add = (add_format_fn)(uintptr_t)dlsym(RTLD_NEXT,
			"weston_drm_format_array_add_format");

	/* weston_drm_format_array_find_format(), inlined by the caller. */
	for (i = 0; i < n; i++) {
		if (fmts[i].format == format)
			return &fmts[i];
	}

	return real_add(formats, format);
}

/*
 * The duplicate format also brings a duplicate modifier with it, so
 * weston_drm_format_add_modifier() asserts next. Same fix: return success
 * without adding, which is what the caller wanted to express. Idempotent, so
 * the first add still goes through to the real function.
 */
int weston_drm_format_add_modifier(struct weston_drm_format *format,
				   uint64_t modifier)
{
	const uint64_t *mods = format->modifiers.data;
	size_t n = format->modifiers.size / sizeof(*mods);
	size_t i;

	if (!real_add_mod)
		real_add_mod = (add_mod_fn)(uintptr_t)dlsym(RTLD_NEXT,
			"weston_drm_format_add_modifier");

	for (i = 0; i < n; i++) {
		if (mods[i] == modifier)
			return 0;
	}

	return real_add_mod(format, modifier);
}
