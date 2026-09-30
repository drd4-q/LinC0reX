// SPDX-License-Identifier: GPL-2.0-only
/*
 * Lindroid display control
 *
 * Copyright (c) 2025 Lindroid Authors
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License v2. See the file COPYING in the main directory of this archive for
 * more details.
 *
 * Lets a plain Linux userspace take the physical panel away from Android and
 * hand it back, without changing the EVDI protocol the Android-side Lindroid
 * composer already speaks.
 */

#include "evdi_drv.h"

#include <linux/device.h>
#include <linux/sched/signal.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#define EVDI_CTL_WAIT_TIMEOUT msecs_to_jiffies(5000)

static DEFINE_MUTEX(evdi_ctl_lock);
static struct evdi_device *evdi_ctl_devices[LINDROID_MAX_CONNECTORS];
static int evdi_ctl_ndev;

static const struct attribute_group evdi_control_attr_group;

/*
 * True when this display is currently allowed to present to the panel.
 *
 * Both the blank state and the handover gate swap delivery: when Linux does
 * not own the panel, or the screen is off, EVDI buffers stop being handed to
 * the composer, which is what makes the Android UI (or nothing at all) show
 * up on the panel.
 */
bool evdi_control_presenting(const struct evdi_display *disp)
{
	return READ_ONCE(disp->owner) == EVDI_OWNER_LINUX &&
	       READ_ONCE(disp->screen) == EVDI_SCREEN_ON &&
	       READ_ONCE(disp->power_mode);
}

void evdi_control_display_init(struct evdi_display *disp)
{
	/*
	 * Default to LINUX so that a freshly booted display behaves exactly
	 * like the driver did before the control ioctls existed: EVDI keeps
	 * feeding the panel.  Handing the panel back to Android is an
	 * explicit act, not something a display does on its own.
	 */
	disp->owner = EVDI_OWNER_LINUX;
	disp->acked_owner = EVDI_OWNER_LINUX;
	disp->screen = EVDI_SCREEN_ON;
	disp->clients = 0;
}

/*
 * Bump the control generation and wake everyone blocked in
 * DRM_IOCTL_EVDI_WAIT_EVENT.  Called with config_mutex held.
 */
void evdi_control_notify(struct evdi_device *evdi)
{
	if (unlikely(!evdi))
		return;

	atomic_inc(&evdi->ctl_generation);
	wake_up_interruptible_poll(&evdi->ctl_wait, EPOLLIN | EPOLLRDNORM);
}

static struct evdi_display *evdi_ctl_display(struct evdi_device *evdi,
					     __s32 display_id)
{
	if (unlikely(!evdi))
		return NULL;

	if (display_id < 0)
		display_id = 0;

	if (unlikely(display_id >= LINDROID_MAX_CONNECTORS))
		return NULL;

	return &evdi->displays[display_id];
}

int evdi_ioctl_control(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct evdi_device *evdi = dev->dev_private;
	struct drm_evdi_control *args = data;
	struct evdi_display *disp;
	int old_value;
	bool changed;

	if (unlikely(atomic_read(&evdi->events.stopping)))
		return -ENODEV;

	disp = evdi_ctl_display(evdi, args->display_id);
	if (unlikely(!disp))
		return -EINVAL;

	changed = false;

	switch (args->cmd) {
	case EVDI_CTL_SET_OWNER:
		if (args->arg != EVDI_OWNER_ANDROID &&
		    args->arg != EVDI_OWNER_LINUX)
			return -EINVAL;

		mutex_lock(&evdi->config_mutex);
		old_value = READ_ONCE(disp->owner);
		WRITE_ONCE(disp->owner, args->arg);
		changed = (old_value != args->arg);
		if (changed) {
			evdi_display_bump_generation(disp);
			evdi_control_notify(evdi);
		}
		mutex_unlock(&evdi->config_mutex);

		if (changed)
			evdi_info("Display %d owner: %s -> %s",
				  args->display_id,
				  old_value == EVDI_OWNER_LINUX ? "linux" : "android",
				  args->arg == EVDI_OWNER_LINUX ? "linux" : "android");
		break;
	case EVDI_CTL_GET_OWNER:
		args->ret = READ_ONCE(disp->owner);
		break;
	case EVDI_CTL_SET_SCREEN:
		if (args->arg != EVDI_SCREEN_ON && args->arg != EVDI_SCREEN_OFF)
			return -EINVAL;

		mutex_lock(&evdi->config_mutex);
		old_value = READ_ONCE(disp->screen);
		WRITE_ONCE(disp->screen, args->arg);
		changed = (old_value != args->arg);
		if (changed) {
			evdi_display_bump_generation(disp);
			evdi_control_notify(evdi);
		}
		mutex_unlock(&evdi->config_mutex);

		if (changed)
			evdi_info("Display %d screen: %s", args->display_id,
				  args->arg == EVDI_SCREEN_ON ? "on" : "off");
		break;
	case EVDI_CTL_GET_SCREEN:
		args->ret = READ_ONCE(disp->screen);
		break;
	case EVDI_CTL_SET_POWER_MODE:
		if (args->arg != 0 && args->arg != 1)
			return -EINVAL;

		mutex_lock(&evdi->config_mutex);
		old_value = READ_ONCE(disp->power_mode);
		WRITE_ONCE(disp->power_mode, args->arg);
		changed = (old_value != args->arg);
		if (changed) {
			evdi_display_bump_generation(disp);
			evdi_control_notify(evdi);
		}
		mutex_unlock(&evdi->config_mutex);
		break;
	case EVDI_CTL_GET_POWER_MODE:
		args->ret = READ_ONCE(disp->power_mode);
		break;
	case EVDI_CTL_ACK_OWNER:
		if (args->arg != EVDI_OWNER_ANDROID &&
		    args->arg != EVDI_OWNER_LINUX)
			return -EINVAL;

		mutex_lock(&evdi->config_mutex);
		old_value = READ_ONCE(disp->acked_owner);
		WRITE_ONCE(disp->acked_owner, args->arg);
		changed = (old_value != args->arg);
		if (changed)
			evdi_control_notify(evdi);
		mutex_unlock(&evdi->config_mutex);

		if (changed)
			evdi_info("Display %d ack owner: %s",
				  args->display_id,
				  args->arg == EVDI_OWNER_LINUX ? "linux" : "android");
		break;
	case EVDI_CTL_SET_GEOMETRY:
		if (args->arg < 640 || args->arg > 8192)
			return -EINVAL;

		mutex_lock(&evdi->config_mutex);
		if (READ_ONCE(disp->width) != (__u32)args->arg) {
			WRITE_ONCE(disp->width, (__u32)args->arg);
			evdi_display_bump_generation(disp);
			evdi_control_notify(evdi);
		}
		mutex_unlock(&evdi->config_mutex);
		break;
	case EVDI_CTL_GET_NDISPLAYS:
		args->ret = LINDROID_MAX_CONNECTORS;
		break;
	default:
		return -EINVAL;
	}

	args->generation = atomic_read(&evdi->ctl_generation);
	return 0;
}

int evdi_ioctl_get_info(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct evdi_device *evdi = dev->dev_private;
	struct drm_evdi_display_info *info = data;
	struct evdi_display *disp;
	int clients = 0;
	int i;

	if (unlikely(atomic_read(&evdi->events.stopping)))
		return -ENODEV;

	disp = evdi_ctl_display(evdi, info->display_id);
	if (unlikely(!disp))
		return -EINVAL;

	for (i = 0; i < LINDROID_MAX_CONNECTORS; i++)
		clients += READ_ONCE(evdi->displays[i].clients);

	info->display_id = disp - evdi->displays;
	info->connected = READ_ONCE(disp->connected);
	info->owner = READ_ONCE(disp->owner);
	info->acked_owner = READ_ONCE(disp->acked_owner);
	info->screen = READ_ONCE(disp->screen);
	info->power_mode = READ_ONCE(disp->power_mode);
	info->clients = clients;
	info->max_displays = LINDROID_MAX_CONNECTORS;
	info->width = READ_ONCE(disp->width);
	info->height = READ_ONCE(disp->height);
	info->refresh_rate = READ_ONCE(disp->refresh_rate);
	info->generation = READ_ONCE(disp->generation);
	info->dev_index = evdi->dev_index;
	info->ctl_generation = atomic_read(&evdi->ctl_generation);
	info->pad = 0;

	return 0;
}

int evdi_ioctl_wait_event(struct drm_device *dev, void *data,
			  struct drm_file *file)
{
	struct evdi_device *evdi = dev->dev_private;
	struct drm_evdi_control *args = data;
	struct evdi_display *disp;
	bool want_owner = args->arg != 0;
	__s32 target = args->arg;
	u64 seen = args->generation;
	long ret;

	disp = evdi_ctl_display(evdi, args->display_id);
	if (unlikely(!disp))
		return -EINVAL;

	if (want_owner && target != EVDI_OWNER_ANDROID &&
	    target != EVDI_OWNER_LINUX)
		return -EINVAL;

	for (;;) {
		u32 gen = atomic_read(&evdi->ctl_generation);
		__s32 owner = READ_ONCE(disp->owner);

		if (atomic_read(&evdi->events.stopping))
			return -ENODEV;

		if (want_owner ? (owner == target) : (gen != seen)) {
			args->ret = owner;
			args->generation = gen;
			return 0;
		}

		if (signal_pending(current))
			return -ERESTARTSYS;

		ret = wait_event_interruptible_timeout(evdi->ctl_wait,
			atomic_read(&evdi->ctl_generation) != seen ||
			atomic_read(&evdi->events.stopping) ||
			(want_owner && READ_ONCE(disp->owner) == target),
			EVDI_CTL_WAIT_TIMEOUT);
		if (ret == 0)
			return -ETIMEDOUT;
		if (ret < 0)
			return (int)ret;
	}
}

/* ------------------------------------------------------------------ */
/* Sysfs front end, so the same state is reachable from plain shell.   */
/* ------------------------------------------------------------------ */

static struct evdi_device *evdi_ctl_any_device(void)
{
	struct evdi_device *evdi;

	mutex_lock(&evdi_ctl_lock);
	evdi = evdi_ctl_ndev ? evdi_ctl_devices[0] : NULL;
	mutex_unlock(&evdi_ctl_lock);

	return evdi;
}

static struct evdi_display *evdi_ctl_show_display(struct evdi_device *evdi)
{
	/*
	 * Report the first display a client has actually driven, so the
	 * sysfs front end stays useful when several displays exist.
	 */
	int i;

	for (i = 0; i < LINDROID_MAX_CONNECTORS; i++)
		if (READ_ONCE(evdi->displays[i].clients))
			return &evdi->displays[i];

	return &evdi->displays[0];
}

static int evdi_ctl_apply(int cmd, __s32 arg)
{
	struct evdi_device *evdi = evdi_ctl_any_device();
	struct evdi_display *disp;
	struct drm_evdi_control ctl;

	if (!evdi)
		return -ENODEV;

	disp = evdi_ctl_show_display(evdi);

	memset(&ctl, 0, sizeof(ctl));
	ctl.cmd = cmd;
	ctl.display_id = (__s32)(disp - evdi->displays);
	ctl.arg = arg;

	return evdi_ioctl_control(evdi->ddev, &ctl, NULL);
}

static ssize_t owner_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct evdi_device *evdi = evdi_ctl_any_device();
	struct evdi_display *disp;

	if (!evdi)
		return -ENODEV;

	disp = evdi_ctl_show_display(evdi);

	return sprintf(buf, "%s\n",
		       READ_ONCE(disp->owner) == EVDI_OWNER_LINUX ?
		       "linux" : "android");
}

static ssize_t owner_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	__s32 arg;
	int val, ret;

	ret = kstrtoint(buf, 0, &val);
	if (!ret) {
		if (val == EVDI_OWNER_LINUX || val == EVDI_OWNER_ANDROID)
			arg = val;
		else
			return -EINVAL;
	} else if (!strncmp(buf, "linux", 5)) {
		arg = EVDI_OWNER_LINUX;
	} else if (!strncmp(buf, "android", 7)) {
		arg = EVDI_OWNER_ANDROID;
	} else {
		return -EINVAL;
	}

	ret = evdi_ctl_apply(EVDI_CTL_SET_OWNER, arg);

	return ret ? ret : (ssize_t)count;
}

/*
 * World-writable, for the same reason the sysfs "add" attribute is (see
 * evdi_sysfs.c): the whole point is to reach this from a normal Linux
 * userspace, and the container-side daemon runs unprivileged.
 */
static struct device_attribute dev_attr_owner_0666 = {
	.attr = {
		.name = "owner",
		.mode = 0666,
	},
	.show = owner_show,
	.store = owner_store,
};

static ssize_t screen_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct evdi_device *evdi = evdi_ctl_any_device();
	struct evdi_display *disp;

	if (!evdi)
		return -ENODEV;

	disp = evdi_ctl_show_display(evdi);

	return sprintf(buf, "%s\n",
		       READ_ONCE(disp->screen) == EVDI_SCREEN_ON ? "on" : "off");
}

static ssize_t screen_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	__s32 arg;
	int val, ret;

	ret = kstrtoint(buf, 0, &val);
	if (!ret) {
		if (val != EVDI_SCREEN_ON && val != EVDI_SCREEN_OFF)
			return -EINVAL;
		arg = val;
	} else if (!strncmp(buf, "on", 2)) {
		arg = EVDI_SCREEN_ON;
	} else if (!strncmp(buf, "off", 3)) {
		arg = EVDI_SCREEN_OFF;
	} else {
		return -EINVAL;
	}

	ret = evdi_ctl_apply(EVDI_CTL_SET_SCREEN, arg);

	return ret ? ret : (ssize_t)count;
}

static struct device_attribute dev_attr_screen_0666 = {
	.attr = {
		.name = "screen",
		.mode = 0666,
	},
	.show = screen_show,
	.store = screen_store,
};

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct evdi_device *evdi = evdi_ctl_any_device();
	struct evdi_display *disp;
	int i, off = 0;

	if (!evdi)
		return -ENODEV;

	for (i = 0; i < LINDROID_MAX_CONNECTORS; i++) {
		disp = &evdi->displays[i];

		off += sprintf(buf + off,
			"display%d: connected=%d owner=%s acked=%s screen=%s "
			"power=%d clients=%d %ux%u@%u gen=%u\n",
			i,
			READ_ONCE(disp->connected),
			READ_ONCE(disp->owner) == EVDI_OWNER_LINUX ? "linux" : "android",
			READ_ONCE(disp->acked_owner) == EVDI_OWNER_LINUX ? "linux" : "android",
			READ_ONCE(disp->screen) == EVDI_SCREEN_ON ? "on" : "off",
			READ_ONCE(disp->power_mode),
			READ_ONCE(disp->clients),
			READ_ONCE(disp->width),
			READ_ONCE(disp->height),
			READ_ONCE(disp->refresh_rate),
			READ_ONCE(disp->generation));
	}

	off += sprintf(buf + off, "dev_index: %d ctl_generation: %u\n",
			evdi->dev_index,
			atomic_read(&evdi->ctl_generation));

	return off;
}

static DEVICE_ATTR_RO(state);

static struct attribute *evdi_control_attrs[] = {
	&dev_attr_owner_0666.attr,
	&dev_attr_screen_0666.attr,
	&dev_attr_state.attr,
	NULL,
};

static const struct attribute_group evdi_control_attr_group = {
	.attrs = evdi_control_attrs,
};

void evdi_control_add_device(struct evdi_device *evdi)
{
	int ret;

	mutex_lock(&evdi_ctl_lock);
	if (evdi_ctl_ndev < LINDROID_MAX_CONNECTORS)
		evdi_ctl_devices[evdi_ctl_ndev++] = evdi;
	mutex_unlock(&evdi_ctl_lock);

	ret = sysfs_create_group(&evdi->ddev->dev->kobj,
				 &evdi_control_attr_group);
	if (ret)
		evdi_err("Failed to create control sysfs group: %d", ret);

	evdi_info("Display control available at /sys/devices/%s/%s%d/",
		  DRIVER_NAME, DRIVER_NAME, evdi->dev_index);
}

void evdi_control_del_device(struct evdi_device *evdi)
{
	int i;

	sysfs_remove_group(&evdi->ddev->dev->kobj,
			   &evdi_control_attr_group);

	mutex_lock(&evdi_ctl_lock);
	for (i = 0; i < evdi_ctl_ndev; i++) {
		if (evdi_ctl_devices[i] == evdi) {
			evdi_ctl_devices[i] =
				evdi_ctl_devices[--evdi_ctl_ndev];
			break;
		}
	}
	mutex_unlock(&evdi_ctl_lock);
}
