// SPDX-License-Identifier: GPL-2.0
/**
 * Bus for USB Type-C Alternate Modes
 *
 * Copyright (C) 2018 Intel Corporation
 * Author: Heikki Krogerus <heikki.krogerus@linux.intel.com>
 */

#include "bus.h"

/* -------------------------------------------------------------------------- */
/* Common API */

/**
 * typec_altmode_notify - Communicate with the platform
 * @adev: Handle to the alternate mode
 * @conf: Alternate mode specific configuration value
 * @data: Alternate mode specific data
 *
 * The primary purpose for this function is to allow the alternate mode drivers
 * to tell the platform which pin configuration has been negotiated with the
 * partner, but communication to the other direction is also possible, so low
 * level device drivers can also send notifications to the alternate mode
 * drivers. The actual communication will be specific for every SVID.
 */
int typec_altmode_notify(struct typec_altmode *adev,
			 unsigned long conf, void *data)
{
	struct altmode *altmode;
	struct altmode *partner;
	int ret;

	/*
	 * All SVID specific configuration values must start from
	 * TYPEC_STATE_MODAL. The first values are reserved for the pin states
	 * defined in USB Type-C specification: TYPEC_STATE_USB and
	 * TYPEC_STATE_SAFE. We'll follow this rule even with modes that do not
	 * require pin reconfiguration for the sake of simplicity.
	 */
	if (conf < TYPEC_STATE_MODAL)
		return -EINVAL;

	if (!adev)
		return 0;

	altmode = to_altmode(adev);

	if (!altmode->partner)
		return -ENODEV;

	partner = altmode->partner;

	ret = typec_set_mode(typec_altmode2port(&partner->adev), (int)conf);
	if (ret)
		return ret;

	blocking_notifier_call_chain(is_typec_port(adev->dev.parent) ?
				     &altmode->nh : &partner->nh,
				     conf, data);

	if (partner->ops && partner->ops->notify)
		return partner->ops->notify(&partner->adev, conf, data);

	return 0;
}
EXPORT_SYMBOL_GPL(typec_altmode_notify);

static int active_match(struct device *dev, void *data)
{
	if (!is_typec_altmode(dev))
		return 0;

	return to_typec_altmode(dev)->active;
}

/**
 * typec_altmode_enter - Enter Mode
 * @adev: The alternate mode
 *
 * The alternate mode drivers use this function enter mode. The port drivers use
 * this to inform the alternate mode driver that their mode has been entered
 * successfully.
 */
int typec_altmode_enter(struct typec_altmode *adev)
{
	struct altmode *altmode = to_altmode(adev);
	struct altmode *partner = altmode->partner;
	struct device *dev;
	int ret;

	/* In case of port, calling the driver and exiting */
	if (is_typec_port(adev->dev.parent)) {
		typec_altmode_update_active(adev, adev->mode, true);
		sysfs_notify(&adev->dev.kobj, NULL, "active");

		if (partner && partner->ops && partner->ops->enter)
			partner->ops->enter(&partner->adev);
		return 0;
	}

	/* REVISIT: Only supporting single mode at a time for now. */
	dev = device_find_child(adev->dev.parent, NULL, active_match);
	if (dev) {
		put_device(dev);
		return -EBUSY;
	}

	/* First moving to USB Safe State */
	ret = typec_set_mode(typec_altmode2port(&partner->adev),
			     TYPEC_STATE_SAFE);
	if (ret)
		return ret;

	blocking_notifier_call_chain(&partner->nh, TYPEC_STATE_SAFE, NULL);

	/* Enter Mode command */
	if (partner->ops && partner->ops->enter)
		partner->ops->enter(&partner->adev);

	return 0;
}
EXPORT_SYMBOL_GPL(typec_altmode_enter);

/**
 * typec_altmode_enter - Exit Mode
 * @adev: The alternate mode
 *
 * The alternate mode drivers use this function to exit mode. The port drivers
 * can also inform the alternate mode drivers with this function that a mode was
 * successfully exited.
 */
int typec_altmode_exit(struct typec_altmode *adev)
{
	struct typec_port *port = typec_altmode2port(adev);
	struct altmode *altmode = to_altmode(adev);
	struct altmode *partner = altmode->partner;
	int ret;

	/* In case of port, calling the driver and exiting */
	if (is_typec_port(adev->dev.parent)) {
		typec_altmode_update_active(adev, adev->mode, false);
		sysfs_notify(&adev->dev.kobj, NULL, "active");

		if (partner && partner->ops && partner->ops->exit)
			partner->ops->exit(&partner->adev);
		return 0;
	}

	/* Moving to USB Safe State */
	ret = typec_set_mode(port, TYPEC_STATE_SAFE);
	if (ret)
		return ret;

	blocking_notifier_call_chain(&partner->nh, TYPEC_STATE_SAFE, NULL);

	/* Exit Mode command */
	if (partner->ops && partner->ops->exit)
		partner->ops->exit(&partner->adev);

	/* Back to USB operation */
	ret = typec_set_mode(port, TYPEC_STATE_USB);
	if (ret)
		return ret;

	blocking_notifier_call_chain(&partner->nh, TYPEC_STATE_USB, NULL);

	return 0;
}
EXPORT_SYMBOL_GPL(typec_altmode_exit);

/**
 * typec_altmode_attention - Attention command
 * @adev: The alternate mode
 * @vdo: VDO for the Attention command
 *
 * Notifies the partner of @adev about Attention command.
 */
void typec_altmode_attention(struct typec_altmode *adev, const u32 vdo)
{
	struct altmode *altmode = to_altmode(adev);
	struct altmode *partner = altmode->partner;

	if (partner && partner->ops && partner->ops->attention)
		partner->ops->attention(&partner->adev, vdo);
}
EXPORT_SYMBOL_GPL(typec_altmode_attention);

/**
 * typec_altmode_vdm - Send Vendor Defined Messages (VDM) to the partner
 * @adev: Alternate mode handle
 * @header: VDM Header
 * @vdo: Array of Vendor Defined Data Objects
 * @count: Number of Data Objects
 *
 * The alternate mode drivers use this function for SVID specific communication
 * with the partner. The port drivers use it to deliver the Structured VDMs
 * received from the partners to the alternate mode drivers.
 */
int typec_altmode_vdm(struct typec_altmode *adev,
		      const u32 header, const u32 *vdo, int count)
{
	struct altmode *altmode;
	struct altmode *partner;

	if (!adev)
		return 0;

	altmode = to_altmode(adev);

	if (!altmode->partner)
		return -ENODEV;

	partner = altmode->partner;

	if (partner->ops && partner->ops->vdm)
		return partner->ops->vdm(&partner->adev, header, vdo, count);

	return 0;
}
EXPORT_SYMBOL_GPL(typec_altmode_vdm);

void typec_altmode_register_ops(struct typec_altmode *adev,
				const struct typec_altmode_ops *ops)
{
	struct altmode *altmode = to_altmode(adev);

	altmode->ops = ops;
}
EXPORT_SYMBOL_GPL(typec_altmode_register_ops);

/* -------------------------------------------------------------------------- */
/* API for the alternate mode drivers */

/**
 * typec_altmode_get_plug - Find cable plug alternate mode
 * @adev: Handle to partner alternate mode
 * @index: Cable plug index
 *
 * Increment reference count for cable plug alternate mode device. Returns
 * handle to the cable plug alternate mode, or NULL if none is found.
 */
struct typec_altmode *typec_altmode_get_plug(struct typec_altmode *adev,
					     int index)
{
	struct altmode *altmode = to_altmode(adev);

	if (altmode->partner->plug[index]) {
		get_device(&altmode->partner->plug[index]->adev.dev);
		return &altmode->partner->plug[index]->adev;
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(typec_altmode_get_plug);

/**
 * typec_altmode_get_plug - Decrement cable plug alternate mode reference count
 * @plug: Handle to the cable plug alternate mode
 */
void typec_altmode_put_plug(struct typec_altmode *plug)
{
	if (plug)
		put_device(&plug->dev);
}
EXPORT_SYMBOL_GPL(typec_altmode_put_plug);

int __typec_altmode_register_driver(struct typec_altmode_driver *drv,
				    struct module *module)
{
	if (!drv->probe)
		return -EINVAL;

	drv->driver.owner = module;
	drv->driver.bus = &typec_bus;

	return driver_register(&drv->driver);
}
EXPORT_SYMBOL_GPL(__typec_altmode_register_driver);

void typec_altmode_unregister_driver(struct typec_altmode_driver *drv)
{
	driver_unregister(&drv->driver);
}
EXPORT_SYMBOL_GPL(typec_altmode_unregister_driver);

/* -------------------------------------------------------------------------- */
/* API for the port drivers */

/**
 * typec_match_altmode - Match SVID to an array of alternate modes
 * @altmodes: Array of alternate modes
 * @n: Number of elements in the array, or -1 for NULL termiated arrays
 * @svid: Standard or Vendor ID to match with
 *
 * Return pointer to an alternate mode with SVID mathing @svid, or NULL when no
 * match is found.
 */
struct typec_altmode *typec_match_altmode(struct typec_altmode **altmodes,
					  size_t n, u16 svid, u8 mode)
{
	int i;

	for (i = 0; i < n; i++) {
		if (!altmodes[i])
			break;
		if (altmodes[i]->svid == svid && altmodes[i]->mode == mode)
			return altmodes[i];
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(typec_match_altmode);

/* -------------------------------------------------------------------------- */

static ssize_t
active_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct typec_altmode *alt = to_typec_altmode(dev);

	return sprintf(buf, "%s\n", alt->active ? "yes" : "no");
}

static ssize_t
active_store(struct device *dev, struct device_attribute *attr,
	     const char *buf, size_t size)
{
	bool activate;
	int ret;

	ret = kstrtobool(buf, &activate);
	if (ret)
		return ret;

	ret = typec_enter_mode(to_altmode(to_typec_altmode(dev)), activate);
	if (ret)
		return ret;

	return size;
}
static DEVICE_ATTR_RW(active);

static ssize_t
description_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct typec_altmode *alt = to_typec_altmode(dev);

	return sprintf(buf, "%s\n", alt->desc ? alt->desc : "");
}
static DEVICE_ATTR_RO(description);

static struct attribute *typec_attrs[] = {
	&dev_attr_active.attr,
	&dev_attr_description.attr,
	NULL
};
ATTRIBUTE_GROUPS(typec);

static int typec_match(struct device *dev, struct device_driver *driver)
{
	struct typec_altmode_driver *drv = to_altmode_driver(driver);
	struct typec_altmode *altmode = to_typec_altmode(dev);
	const struct typec_device_id *id;

	for (id = drv->id_table; id->svid; id++)
		if ((id->svid == altmode->svid) &&
		    (id->mode == TYPEC_ANY_MODE || id->mode == altmode->mode))
			return 1;
	return 0;
}

static int typec_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	struct typec_altmode *altmode = to_typec_altmode(dev);

	if (add_uevent_var(env, "SVID=%04X", altmode->svid))
		return -ENOMEM;

	if (add_uevent_var(env, "MODE=%u", altmode->mode))
		return -ENOMEM;

	return add_uevent_var(env, "MODALIAS=typec:id%04Xm%02X",
			      altmode->svid, altmode->mode);
}

static int typec_probe(struct device *dev)
{
	struct typec_altmode_driver *drv = to_altmode_driver(dev->driver);
	struct typec_altmode *adev = to_typec_altmode(dev);
	struct altmode *altmode = to_altmode(adev);

	/* Fail if the port does not support the alternate mode */
	if (!altmode->partner)
		return -ENODEV;

	return drv->probe(adev, altmode->partner->adev.vdo);
}

static int typec_remove(struct device *dev)
{
	struct typec_altmode_driver *drv = to_altmode_driver(dev->driver);

	if (drv->remove)
		drv->remove(to_typec_altmode(dev));

	return 0;
}

struct bus_type typec_bus = {
	.name = "typec",
	.dev_groups = typec_groups,
	.match = typec_match,
	.uevent = typec_uevent,
	.probe = typec_probe,
	.remove = typec_remove,
};
