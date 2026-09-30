// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 Red Hat
 * Copyright (c) 2015 - 2020 DisplayLink (UK) Ltd.
 * Copyright (c) 2025 Lindroid Authors
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License v2. See the file COPYING in the main directory of this archive for
 * more details.
 */

#ifndef __UAPI_EVDI_DRM_H__
#define __UAPI_EVDI_DRM_H__

#ifdef __KERNEL__
#include <linux/types.h>
#include <drm/drm.h>
#else
#include <stdint.h>
#include <drm/drm.h>
#endif

enum poll_event_type {
	none = 0,
	add_buf = 1,
	get_buf = 2,
	destroy_buf = 3,
	swap_to = 4,
	create_buf = 5
};

struct drm_evdi_connect {
	int32_t connected;
	int32_t dev_index;
	uint32_t width;
	uint32_t height;
	uint32_t refresh_rate;
	uint32_t display_id;
};

struct drm_evdi_poll {
	enum poll_event_type event;
	int poll_id;
	void *data;
};

struct drm_evdi_get_buff_callabck {
	int poll_id;
	int version;
	int numFds;
	int numInts;
	int *fd_ints;
	int *data_ints;
};

struct drm_evdi_destroy_buff_callback {
	int poll_id;
};

struct drm_evdi_create_buff_callabck {
	int poll_id;
	int id;
	uint32_t stride;
};

struct drm_evdi_gbm_create_buff {
	int *id;
	uint32_t *stride;
	uint32_t format;
	uint32_t width;
	uint32_t height;
};

struct drm_evdi_gbm_get_buff {
	int id;
	void *native_handle;
};

struct drm_evdi_gbm_del_buff {
	int id;
};

struct drm_evdi_vsync {
    __u32 display_id;
};

struct drm_evdi_set_power_mode {
    __s32 display_id;
    __s32 power_mode;
};

/*
 * Lindroid display control
 * ------------------------
 * Who currently owns the physical panel and whether it is lit at all.
 *
 * The panel itself is scanned out by Android userspace, so a plain Linux
 * userspace cannot reprogram the display hardware directly.  What it *can*
 * do is drive the EVDI virtual display that the Lindroid composer hands to
 * the panel.  These ioctls are the switch:
 *
 *   owner = LINUX   - EVDI buffers are scanned out, Android UI is dropped
 *   owner = ANDROID - EVDI stops presenting, Android UI comes back
 *   screen = OFF    - nothing is presented at all, display is blanked
 *
 * They are intentionally additive: the ioctls above are untouched so an
 * already shipped Lindroid composer keeps working unchanged.
 */

/** Which side of the handover currently feeds the panel. */
enum drm_evdi_owner {
	EVDI_OWNER_ANDROID	= 0,
	EVDI_OWNER_LINUX	= 1,
};

/** Whether the display is lit. */
enum drm_evdi_screen {
	EVDI_SCREEN_OFF		= 0,
	EVDI_SCREEN_ON		= 1,
};

/** Commands for DRM_IOCTL_EVDI_CONTROL. */
enum drm_evdi_ctl_cmd {
	/** arg = enum drm_evdi_owner, request the Android <-> Linux handover. */
	EVDI_CTL_SET_OWNER	= 1,
	/** ret = enum drm_evdi_owner, current owner. */
	EVDI_CTL_GET_OWNER	= 2,
	/** arg = enum drm_evdi_screen, blank or light the display. */
	EVDI_CTL_SET_SCREEN	= 3,
	/** ret = enum drm_evdi_screen. */
	EVDI_CTL_GET_SCREEN	= 4,
	/** arg = 0|1, per-display power mode (same as SET_POWER_MODE). */
	EVDI_CTL_SET_POWER_MODE	= 5,
	/** ret = per-display power mode. */
	EVDI_CTL_GET_POWER_MODE	= 6,
	/**
	 * Android side confirms it has completed the handover.
	 * arg = enum drm_evdi_owner that is now actually on the panel.
	 */
	EVDI_CTL_ACK_OWNER	= 7,
	/**
	 * Linux compositor reports the mode it is driving.
	 * arg = width, and generation is unused; see DRM_IOCTL_EVDI_CONNECT.
	 */
	EVDI_CTL_SET_GEOMETRY	= 8,
	/** ret = number of displays known to the driver. */
	EVDI_CTL_GET_NDISPLAYS	= 9,
};

/**
 * How many EVDI displays the driver creates at init.
 *
 * 0 means "none", and that is the required default. DRM minors are handed out
 * in registration order and the Qualcomm display driver registers first, so an
 * EVDI card created at boot would claim /dev/dri/card0 and push the real panel
 * to card1. Android hardcodes card0 in SurfaceFlinger and gralloc, which leaves
 * the phone stuck on the boot logo with no SurfaceFlinger at all.
 *
 * Override with the "evdi.evdi_nr_devices=" kernel command line.
 */
#define EVDI_DEFAULT_NR_DEVICES 0

struct drm_evdi_control {
	__s32	cmd;
	__s32	display_id;
	__s32	arg;
	__s32	ret;
	__u64	generation;
};

struct drm_evdi_display_info {
	__s32	display_id;
	__s32	connected;
	__s32	owner;
	__s32	acked_owner;
	__s32	screen;
	__s32	power_mode;
	__s32	clients;
	__s32	max_displays;
	__u32	width;
	__u32	height;
	__u32	refresh_rate;
	__u32	generation;
	__u32	dev_index;
	__u32	ctl_generation;
	__u32	pad;
};

#define DRM_EVDI_CONNECT                    0x00
#define DRM_EVDI_GRABPIX                    0x02  /* Unused by create-disp */
#define DRM_EVDI_ENABLE_CURSOR_EVENTS       0x03  /* Unused by create-disp */
#define DRM_EVDI_POLL                       0x04
#define DRM_EVDI_GBM_ADD_BUFF               0x05  /* Unused by create-disp */
#define DRM_EVDI_GBM_GET_BUFF               0x06  /* Unused by create-disp */
#define DRM_EVDI_GET_BUFF_CALLBACK          0x08
#define DRM_EVDI_DESTROY_BUFF_CALLBACK      0x09
#define DRM_EVDI_GBM_DEL_BUFF               0x0B  /* Unused by create-disp */
#define DRM_EVDI_GBM_CREATE_BUFF            0x0C  /* Unused by create-disp */
#define DRM_EVDI_GBM_CREATE_BUFF_CALLBACK   0x0D
#define DRM_EVDI_VSYNC						0x0E
#define DRM_EVDI_SET_POWER_MODE             0x0F
#define DRM_EVDI_CONTROL                    0x10
#define DRM_EVDI_GET_INFO                  0x11
#define DRM_EVDI_WAIT_EVENT                 0x12

#define DRM_IOCTL_EVDI_CONNECT DRM_IOWR(DRM_COMMAND_BASE + \
	DRM_EVDI_CONNECT, struct drm_evdi_connect)

#define DRM_IOCTL_EVDI_POLL DRM_IOWR(DRM_COMMAND_BASE + \
	DRM_EVDI_POLL, struct drm_evdi_poll)

#define DRM_IOCTL_EVDI_GBM_GET_BUFF DRM_IOWR(DRM_COMMAND_BASE +  \
			DRM_EVDI_GBM_GET_BUFF, struct drm_evdi_gbm_get_buff)

#define DRM_IOCTL_EVDI_GET_BUFF_CALLBACK DRM_IOWR(DRM_COMMAND_BASE + \
	DRM_EVDI_GET_BUFF_CALLBACK, struct drm_evdi_get_buff_callabck)

#define DRM_IOCTL_EVDI_DESTROY_BUFF_CALLBACK DRM_IOWR(DRM_COMMAND_BASE + \
	DRM_EVDI_DESTROY_BUFF_CALLBACK, struct drm_evdi_destroy_buff_callback)

#define DRM_IOCTL_EVDI_GBM_CREATE_BUFF DRM_IOWR(DRM_COMMAND_BASE +  \
			DRM_EVDI_GBM_CREATE_BUFF, struct drm_evdi_gbm_create_buff)

#define DRM_IOCTL_EVDI_GBM_CREATE_BUFF_CALLBACK DRM_IOWR(DRM_COMMAND_BASE + \
	DRM_EVDI_GBM_CREATE_BUFF_CALLBACK, struct drm_evdi_create_buff_callabck)

#define DRM_IOCTL_EVDI_GBM_DEL_BUFF DRM_IOWR(DRM_COMMAND_BASE + \
	DRM_EVDI_GBM_DEL_BUFF, struct drm_evdi_gbm_del_buff)

#define DRM_IOCTL_EVDI_VSYNC DRM_IOW(DRM_COMMAND_BASE + \
	DRM_EVDI_VSYNC, struct drm_evdi_vsync)

#define DRM_IOCTL_EVDI_SET_POWER_MODE DRM_IOW(DRM_COMMAND_BASE + \
	DRM_EVDI_SET_POWER_MODE, struct drm_evdi_set_power_mode)

#define DRM_IOCTL_EVDI_CONTROL DRM_IOWR(DRM_COMMAND_BASE + \
	DRM_EVDI_CONTROL, struct drm_evdi_control)

#define DRM_IOCTL_EVDI_GET_INFO DRM_IOWR(DRM_COMMAND_BASE + \
	DRM_EVDI_GET_INFO, struct drm_evdi_display_info)

#define DRM_IOCTL_EVDI_WAIT_EVENT DRM_IOWR(DRM_COMMAND_BASE + \
	DRM_EVDI_WAIT_EVENT, struct drm_evdi_control)

#endif /* __UAPI_EVDI_DRM_H__ */
