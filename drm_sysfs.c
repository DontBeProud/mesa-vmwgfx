
/*
 * drm_sysfs.c - Modifications to drm_sysfs_class.c to support
 *               extra sysfs attribute from DRM. Normal drm_sysfs_class
 *               does not allow adding attributes.
 *
 * Copyright (c) 2004 Jon Smirl <jonsmirl@gmail.com>
 * Copyright (c) 2003-2004 Greg Kroah-Hartman <greg@kroah.com>
 * Copyright (c) 2003-2004 IBM Corp.
 *
 * This file is released under the GPLv2
 *
 */

#include <linux/device.h>
#include <linux/kdev_t.h>
#include <linux/gfp.h>
#include <linux/err.h>
#include <linux/export.h>

#include "drm_sysfs.h"
#include "drm_core.h"
#include "drmP.h"
#include "drm_internal.h"

#define to_drm_minor(d) dev_get_drvdata(d)
#define to_drm_connector(d) dev_get_drvdata(d)

static void drm_sysfs_release(struct device *dev);


static struct device_type drm_sysfs_device_minor = {
	.name = "drm_minor"
};


/*
 * Connector properties
 */
static ssize_t status_store(struct device *device,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct drm_connector *connector = to_drm_connector(device);
	struct drm_device *dev = connector->dev;
	enum drm_connector_force old_force;
	int ret;

	ret = mutex_lock_interruptible(&dev->mode_config.mutex);
	if (ret)
		return ret;

	old_force = connector->force;

	if (sysfs_streq(buf, "detect"))
		connector->force = 0;
	else if (sysfs_streq(buf, "on"))
		connector->force = DRM_FORCE_ON;
	else if (sysfs_streq(buf, "on-digital"))
		connector->force = DRM_FORCE_ON_DIGITAL;
	else if (sysfs_streq(buf, "off"))
		connector->force = DRM_FORCE_OFF;
	else
		ret = -EINVAL;

	if (old_force != connector->force || !connector->force) {
		DRM_DEBUG_KMS("[CONNECTOR:%d:%s] force updated from %d to %d or reprobing\n",
			      connector->base.id,
			      connector->name,
			      old_force, connector->force);

		connector->funcs->fill_modes(connector,
					     dev->mode_config.max_width,
					     dev->mode_config.max_height);
	}

	mutex_unlock(&dev->mode_config.mutex);

	return ret ? ret : count;
}

static ssize_t status_show(struct device *device,
			   struct device_attribute *attr,
			   char *buf)
{
	struct drm_connector *connector = to_drm_connector(device);
	enum drm_connector_status status;

	status = READ_ONCE(connector->status);

	return snprintf(buf, PAGE_SIZE, "%s\n",
			drm_get_connector_status_name(status));
}

static ssize_t dpms_show(struct device *device,
			   struct device_attribute *attr,
			   char *buf)
{
	struct drm_connector *connector = to_drm_connector(device);
	int dpms;

	dpms = READ_ONCE(connector->dpms);

	return snprintf(buf, PAGE_SIZE, "%s\n",
			drm_get_dpms_name(dpms));
}

static ssize_t enabled_show(struct device *device,
			    struct device_attribute *attr,
			   char *buf)
{
	struct drm_connector *connector = to_drm_connector(device);
	bool enabled;

	enabled = READ_ONCE(connector->encoder);

	return snprintf(buf, PAGE_SIZE, enabled ? "enabled\n" : "disabled\n");
}

static ssize_t edid_show(
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 35) || \
     RHEL_VERSION_CODE >= RHEL_RELEASE_VERSION(6, 5))
                         struct file *file,
#endif
                         struct kobject *kobj, struct bin_attribute *attr,
			 char *buf, loff_t off, size_t count)
{
	struct device *connector_dev = kobj_to_dev(kobj);
	struct drm_connector *connector = to_drm_connector(connector_dev);
	unsigned char *edid;
	size_t size;
	ssize_t ret = 0;

	mutex_lock(&connector->dev->mode_config.mutex);
	if (!connector->edid_blob_ptr)
		goto unlock;

	edid = connector->edid_blob_ptr->data;
	size = connector->edid_blob_ptr->length;
	if (!edid)
		goto unlock;

	if (off >= size)
		goto unlock;

	if (off + count > size)
		count = size - off;
	memcpy(buf, edid + off, count);

	ret = count;
unlock:
	mutex_unlock(&connector->dev->mode_config.mutex);

	return ret;
}

static ssize_t modes_show(struct device *device,
			   struct device_attribute *attr,
			   char *buf)
{
	struct drm_connector *connector = to_drm_connector(device);
	struct drm_display_mode *mode;
	int written = 0;

	mutex_lock(&connector->dev->mode_config.mutex);
	list_for_each_entry(mode, &connector->modes, head) {
		written += snprintf(buf + written, PAGE_SIZE - written, "%s\n",
				    mode->name);
	}
	mutex_unlock(&connector->dev->mode_config.mutex);

	return written;
}

static DEVICE_ATTR_RW(status);
static DEVICE_ATTR_RO(enabled);
static DEVICE_ATTR_RO(dpms);
static DEVICE_ATTR_RO(modes);

static struct attribute *connector_dev_attrs[] = {
	&dev_attr_status.attr,
	&dev_attr_enabled.attr,
	&dev_attr_dpms.attr,
	&dev_attr_modes.attr,
	NULL
};

static struct bin_attribute edid_attr = {
	.attr.name = "edid",
	.attr.mode = 0444,
	.size = 0,
	.read = edid_show,
};

static struct bin_attribute *connector_bin_attrs[] = {
	&edid_attr,
	NULL
};

static const struct attribute_group connector_dev_group = {
	.attrs = connector_dev_attrs,
	.bin_attrs = connector_bin_attrs,
};

static const struct attribute_group *connector_dev_groups[] = {
	&connector_dev_group,
	NULL
};

/**
 * drm_sysfs_connector_add - add a connector to sysfs
 * @connector: connector to add
 *
 * Create a connector device in sysfs, along with its associated connector
 * properties (so far, connection status, dpms, mode list & edid) and
 * generate a hotplug event so userspace knows there's a new connector
 * available.
 */
int drm_sysfs_connector_add(struct drm_connector *connector)
{
	struct drm_device *dev = connector->dev;
	struct device *kdev;
	int retval = -ENODEV;

	if (connector->kdev)
		return 0;

	kdev = kzalloc(sizeof(*kdev), GFP_KERNEL);
	if (!kdev) {
		return -ENOMEM;
	}

	/*
	 * Cannot call device_create_with_groups because we have no access
	 * to drm_class.  Instead, do as much as we can
	 */
	kdev->devt = 0;
	kdev->parent = dev->primary->kdev;
	kdev->groups = connector_dev_groups;
	kdev->type = &drm_sysfs_device_minor;
	kdev->release = drm_sysfs_release;
	dev_set_drvdata(kdev, connector);

	retval = dev_set_name(kdev, "card%d-%s",
			      dev->primary->index,
			      connector->name);
	if (retval) {
		DRM_ERROR("Cannot set device name\n");
		goto error;
	}

	retval = drm_class_device_register(kdev);
	if (retval) {
		DRM_ERROR("failed to register connector device: %ld\n", PTR_ERR(connector->kdev));
		goto error;
	}

	connector->kdev = kdev;

	DRM_DEBUG("adding \"%s\" to sysfs\n", connector->name);

	/* Let userspace know we have a new connector */
	drm_sysfs_hotplug_event(dev);

	return retval;

error:
	kfree(kdev);
	return retval;
}

/**
 * drm_sysfs_connector_remove - remove an connector device from sysfs
 * @connector: connector to remove
 *
 * Remove @connector and its associated attributes from sysfs.  Note that
 * the device model core will take care of sending the "remove" uevent
 * at this time, so we don't need to do it.
 *
 * Note:
 * This routine should only be called if the connector was previously
 * successfully registered.  If @connector hasn't been registered yet,
 * you'll likely see a panic somewhere deep in sysfs code when called.
 */
void drm_sysfs_connector_remove(struct drm_connector *connector)
{
	if (!connector->kdev)
		return;
	DRM_DEBUG("removing \"%s\" from sysfs\n",
		  connector->name);

	/* vwmgfx: We used drm_class_device_register() to register this device */
	drm_class_device_unregister(connector->kdev);
	connector->kdev = NULL;
}

/**
 * drm_sysfs_hotplug_event - generate a DRM uevent
 * @dev: DRM device
 *
 * Send a uevent for the DRM device specified by @dev.  Currently we only
 * set HOTPLUG=1 in the uevent environment, but this could be expanded to
 * deal with other types of events.
 */
void drm_sysfs_hotplug_event(struct drm_device *dev)
{
	char *event_string = "HOTPLUG=1";
	char *envp[] = { event_string, NULL };

	DRM_DEBUG("generating hotplug event\n");

	kobject_uevent_env(&dev->primary->kdev->kobj, KOBJ_CHANGE, envp);
}
EXPORT_SYMBOL(drm_sysfs_hotplug_event);

static void drm_sysfs_release(struct device *dev)
{
	kfree(dev);
}

/**
 * drm_sysfs_minor_alloc() - Allocate sysfs device for given minor
 * @minor: minor to allocate sysfs device for
 *
 * vmwgfx: unlike the real DRM, we have to call drm_class_device_register
 * rather than device_initialize() followed by a device_add() because
 * we have no other way to get the drm_class.
 *
 * Note that dev_get_drvdata() on the new device will return the minor.
 * However, the device does not hold a ref-count to the minor nor to the
 * underlying drm_device. This is unproblematic as long as you access the
 * private data only in sysfs callbacks. device_del() disables those
 * synchronously, so they cannot be called after you cleanup a minor.
 */
struct device *drm_sysfs_minor_alloc(struct drm_minor *minor)
{
	const char *minor_str;
	struct device *kdev;
	int r;

	if (minor->type == DRM_MINOR_CONTROL)
		minor_str = "controlD%d";
	else if (minor->type == DRM_MINOR_RENDER)
		minor_str = "renderD%d";
	else
		minor_str = "card%d";

	kdev = kzalloc(sizeof(*kdev), GFP_KERNEL);
	if (!kdev)
		return ERR_PTR(-ENOMEM);

	kdev->devt = MKDEV(MAJOR(drm_chr_dev), minor->index);
	kdev->type = &drm_sysfs_device_minor;
	kdev->parent = minor->dev->dev;
	kdev->release = drm_sysfs_release;
	dev_set_drvdata(kdev, minor);

	r = dev_set_name(kdev, minor_str, minor->index);
	if (r < 0)
		goto err_free;

	/*
	 * vmwgfx: Register the device here since we cannot do the
	 * device_initialize() -> device_add() sequence.  And because we are
	 * not calling device_initialize() ourselves, we now need to add an
	 * additional reference by calling get_device() so that
	 * drm_sysfs_minor_free() can reduce the reference count back to 0.
	 */
	r = drm_class_device_register(kdev);
	if (r) {
		DRM_ERROR("Failed to register connector device: %d\n", r);
		goto err_free;
	}

	(void *) get_device(kdev);

	return kdev;

err_free:
	return ERR_PTR(r);
}

