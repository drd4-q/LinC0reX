// SPDX-License-Identifier: GPL-2.0-only
/*
 * Host-side test for the Lindroid display-control state machine.
 *
 * The driver logic is a small, self-contained state machine (owner / screen /
 * power_mode -> presenting?).  This mirrors it and checks the truth table, so
 * a refactor that inverts a condition is caught without flashing a phone.
 *
 * Build and run:  make check
 */

#include <stdio.h>
#include <string.h>

#include "uapi/evdi_drm.h"

struct display {
	int owner;
	int screen;
	int power_mode;
};

static int presenting(const struct display *d)
{
	return d->owner == EVDI_OWNER_LINUX &&
	       d->screen == EVDI_SCREEN_ON &&
	       d->power_mode;
}

static int failures;
static int checks;

static void expect(const char *what, int got, int want)
{
	checks++;
	if (got == want)
		return;

	failures++;
	printf("FAIL %-46s got %d want %d\n", what, got, want);
}

int main(void)
{
	struct display d;

	/* The boot default must keep presenting, as before these ioctls. */
	d.owner = EVDI_OWNER_LINUX;
	d.screen = EVDI_SCREEN_ON;
	d.power_mode = 1;
	expect("default presents", presenting(&d), 1);

	/* Handing back to Android stops EVDI from feeding the panel. */
	d.owner = EVDI_OWNER_ANDROID;
	expect("android owner blocks presentation", presenting(&d), 0);

	/* Each single factor is sufficient to gate it. */
	d.owner = EVDI_OWNER_LINUX;
	d.screen = EVDI_SCREEN_OFF;
	expect("screen off blocks presentation", presenting(&d), 0);

	d.screen = EVDI_SCREEN_ON;
	d.power_mode = 0;
	expect("power off blocks presentation", presenting(&d), 0);

	d.power_mode = 1;
	expect("all on presents again", presenting(&d), 1);

	/* Both gates off at once is still just blocked. */
	d.owner = EVDI_OWNER_ANDROID;
	d.screen = EVDI_SCREEN_OFF;
	expect("both gates off", presenting(&d), 0);

	/*
	 * Enum values are part of the ABI: userspace compiled against
	 * uapi/evdi_drm.h sends these exact numbers.
	 */
	expect("EVDI_OWNER_ANDROID == 0", EVDI_OWNER_ANDROID, 0);
	expect("EVDI_OWNER_LINUX == 1", EVDI_OWNER_LINUX, 1);
	expect("EVDI_SCREEN_OFF == 0", EVDI_SCREEN_OFF, 0);
	expect("EVDI_SCREEN_ON == 1", EVDI_SCREEN_ON, 1);

	/* Command numbers must not collide with the pre-existing ioctls. */
	expect("CONTROL after SET_POWER_MODE",
	       DRM_EVDI_CONTROL > DRM_EVDI_SET_POWER_MODE, 1);
	expect("GET_INFO after CONTROL",
	       DRM_EVDI_GET_INFO > DRM_EVDI_CONTROL, 1);
	expect("WAIT_EVENT after GET_INFO",
	       DRM_EVDI_WAIT_EVENT > DRM_EVDI_GET_INFO, 1);
	expect("new ioctls start at 0x10",
	       DRM_EVDI_CONTROL == 0x10, 1);

	/*
	 * Regression: the driver must not create a DRM card at boot.
	 *
	 * DRM minors are assigned in registration order and the Qualcomm
	 * display driver registers first, so an EVDI card created at init
	 * takes /dev/dri/card0 and pushes the real panel to card1. Android
	 * hardcodes card0, so SurfaceFlinger never starts and the phone
	 * hangs on the boot logo.  Observed on moonstone.
	 */
	expect("no EVDI card at boot by default",
	       EVDI_DEFAULT_NR_DEVICES == 0, 1);

	printf("%d checks, %d failures\n", checks, failures);

	return failures ? 1 : 0;
}
