// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2021 Severin von Wnuck <severinvonw@outlook.de>
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/idr.h>
#include <linux/version.h>

#include "bus.h"

#define to_gip_adapter(d) container_of(d, struct gip_adapter, dev)
#define to_gip_client(d) container_of(d, struct gip_client, dev)
#define to_gip_driver(d) container_of(d, struct gip_driver, drv)

static DEFINE_IDA(gip_adapter_ida);

static void gip_adapter_release(struct device *dev)
{
	kfree(to_gip_adapter(dev));
}

static struct device_type gip_adapter_type = {
	.release = gip_adapter_release,
};

static void gip_add_client(struct gip_client *client)
{
	int err;

	err = device_add(&client->dev);
	if (err) {
		dev_err(&client->dev, "%s: add device failed: %d\n",
			__func__, err);
		return;
	}

	dev_dbg(&client->dev, "%s: added\n", __func__);
}

static void gip_remove_client(struct gip_client *client)
{
	dev_dbg(&client->dev, "%s: removed\n", __func__);

	if (device_is_registered(&client->dev))
		device_del(&client->dev);

	put_device(&client->dev);
}

static void gip_client_state_changed(struct work_struct *work)
{
	struct gip_client *client = container_of(work, typeof(*client),
						 state_work);

	switch (atomic_read(&client->state)) {
	case GIP_CL_IDENTIFIED:
		gip_add_client(client);
		break;
	case GIP_CL_DISCONNECTED:
		gip_remove_client(client);
		break;
	default:
		dev_warn(&client->dev, "%s: invalid state\n", __func__);
		break;
	}
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
static int gip_client_uevent(struct device *dev, struct kobj_uevent_env *env)
#else
static int gip_client_uevent(const struct device *dev, struct kobj_uevent_env *env)
#endif
{
	struct gip_client *client = to_gip_client(dev);
	struct gip_classes *classes = client->classes;

	if (!classes || !classes->count)
		return -EINVAL;

	return add_uevent_var(env, "MODALIAS=gip:%s", classes->strings[0]);
}

static void gip_client_release(struct device *dev)
{
	struct gip_client *client = to_gip_client(dev);

	gip_free_client_info(client);
	kfree(client->chunk_buf);
	kfree(client);
}

static struct device_type gip_client_type = {
	.uevent = gip_client_uevent,
	.release = gip_client_release,
};

static int gip_bus_match(struct device *dev, struct device_driver *driver)
{
	struct gip_client *client;
	struct gip_driver *drv;
	int i;

	if (dev->type != &gip_client_type)
		return false;

	client = to_gip_client(dev);
	drv = to_gip_driver(driver);

	for (i = 0; i < client->classes->count; i++)
		if (!strcmp(client->classes->strings[i], drv->class))
			return true;

	return false;
}

static int gip_bus_probe(struct device *dev)
{
	struct gip_client *client = to_gip_client(dev);
	struct gip_driver *drv = to_gip_driver(dev->driver);
	int err;
	unsigned long flags;

	if (client->drv)
		return 0;

	err = drv->probe(client);
	if (!err) {
		spin_lock_irqsave(&client->lock, flags);
		client->drv = drv;
		spin_unlock_irqrestore(&client->lock, flags);
	}

	return err;
}

static void gip_bus_remove(struct device *dev)
{
	struct gip_client *client = to_gip_client(dev);
	struct gip_driver *drv = client->drv;
	unsigned long flags;

	if (!drv)
		return;

	spin_lock_irqsave(&client->lock, flags);
	client->drv = NULL;
	spin_unlock_irqrestore(&client->lock, flags);

	if (drv->remove)
		drv->remove(client);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
static int gip_bus_remove_compat(struct device *dev)
{
	gip_bus_remove(dev);

	return 0;
}
#endif

static struct bus_type gip_bus_type = {
	.name = "xone-gip",
	.match = gip_bus_match,
	.probe = gip_bus_probe,
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
	.remove = gip_bus_remove_compat,
#else
	.remove = gip_bus_remove,
#endif
};

struct gip_adapter *gip_create_adapter(struct device *parent,
				       struct gip_adapter_ops *ops,
				       int audio_pkts)
{
	struct gip_adapter *adap;
	int err;

	adap = kzalloc(sizeof(*adap), GFP_KERNEL);
	if (!adap)
		return ERR_PTR(-ENOMEM);

	adap->id = ida_simple_get(&gip_adapter_ida, 0, 0, GFP_KERNEL);
	if (adap->id < 0) {
		err = adap->id;
		goto err_put_device;
	}

	adap->state_queue = alloc_ordered_workqueue("gip%d", 0, adap->id);
	if (!adap->state_queue) {
		err = -ENOMEM;
		goto err_remove_ida;
	}

	adap->dev.parent = parent;
	adap->dev.type = &gip_adapter_type;
	adap->dev.bus = &gip_bus_type;
	adap->ops = ops;
	adap->audio_packet_count = audio_pkts;
	dev_set_name(&adap->dev, "gip%d", adap->id);
	spin_lock_init(&adap->clients_lock);
	spin_lock_init(&adap->send_lock);

	err = device_register(&adap->dev);
	if (err)
		goto err_destroy_queue;

	dev_dbg(&adap->dev, "%s: registered\n", __func__);

	return adap;

err_destroy_queue:
	destroy_workqueue(adap->state_queue);
err_remove_ida:
	ida_simple_remove(&gip_adapter_ida, adap->id);
err_put_device:
	put_device(&adap->dev);

	return ERR_PTR(err);
}
EXPORT_SYMBOL_GPL(gip_create_adapter);

int gip_power_off_adapter(struct gip_adapter *adap)
{
	struct gip_client *client = adap->clients[0];

	if (!client)
		return 0;

	/* power off main client */
	return gip_set_power_mode(client, GIP_PWR_OFF);
}
EXPORT_SYMBOL_GPL(gip_power_off_adapter);

void gip_destroy_adapter(struct gip_adapter *adap)
{
	struct gip_client *client;
	int i;

	/* ensure all state changes have been processed */
	flush_workqueue(adap->state_queue);

	for (i = GIP_MAX_CLIENTS - 1; i >= 0; i--) {
		client = adap->clients[i];
		if (!client)
			continue;

		gip_remove_client(client);
		adap->clients[i] = NULL;
	}

	ida_simple_remove(&gip_adapter_ida, adap->id);
	destroy_workqueue(adap->state_queue);

	dev_dbg(&adap->dev, "%s: unregistered\n", __func__);
	device_unregister(&adap->dev);
}
EXPORT_SYMBOL_GPL(gip_destroy_adapter);

static struct gip_client *gip_init_client(struct gip_adapter *adap, u8 id)
{
	struct gip_client *client;

	client = kzalloc(sizeof(*client), GFP_ATOMIC);
	if (!client)
		return ERR_PTR(-ENOMEM);

	client->dev.parent = &adap->dev;
	client->dev.type = &gip_client_type;
	client->dev.bus = &gip_bus_type;
	client->id = id;
	client->adapter = adap;
	dev_set_name(&client->dev, "gip%d.%u", adap->id, client->id);
	atomic_set(&client->state, GIP_CL_CONNECTED);
	spin_lock_init(&client->lock);
	INIT_WORK(&client->state_work, gip_client_state_changed);

	device_initialize(&client->dev);
	dev_dbg(&client->dev, "%s: initialized\n", __func__);

	return client;
}

struct gip_client *gip_get_or_init_client(struct gip_adapter *adap, u8 id)
{
	struct gip_client *client;
	unsigned long flags;

	spin_lock_irqsave(&adap->clients_lock, flags);

	client = adap->clients[id];
	if (!client) {
		client = gip_init_client(adap, id);
		if (IS_ERR(client))
			goto err_unlock;

		adap->clients[id] = client;
	}

	get_device(&client->dev);

err_unlock:
	spin_unlock_irqrestore(&adap->clients_lock, flags);

	return client;
}

void gip_put_client(struct gip_client *client)
{
	put_device(&client->dev);
}

void gip_register_client(struct gip_client *client)
{
	atomic_set(&client->state, GIP_CL_IDENTIFIED);
	queue_work(client->adapter->state_queue, &client->state_work);
}

void gip_unregister_client(struct gip_client *client)
{
	struct gip_adapter *adap = client->adapter;
	unsigned long flags;

	spin_lock_irqsave(&adap->clients_lock, flags);
	adap->clients[client->id] = NULL;
	spin_unlock_irqrestore(&adap->clients_lock, flags);

	atomic_set(&client->state, GIP_CL_DISCONNECTED);
	queue_work(adap->state_queue, &client->state_work);
}

void gip_free_client_info(struct gip_client *client)
{
	int i;

	kfree(client->external_commands);
	kfree(client->firmware_versions);
	kfree(client->audio_formats);
	kfree(client->capabilities_out);
	kfree(client->capabilities_in);

	if (client->classes)
		for (i = 0; i < client->classes->count; i++)
			kfree(client->classes->strings[i]);

	kfree(client->classes);
	kfree(client->interfaces);
	kfree(client->hid_descriptor);

	client->external_commands = NULL;
	client->audio_formats = NULL;
	client->capabilities_out = NULL;
	client->capabilities_in = NULL;
	client->classes = NULL;
	client->interfaces = NULL;
	client->hid_descriptor = NULL;
}

int __gip_register_driver(struct gip_driver *drv, struct module *owner,
			  const char *mod_name)
{
	drv->drv.name = drv->name;
	drv->drv.bus = &gip_bus_type;
	drv->drv.owner = owner;
	drv->drv.mod_name = mod_name;

	return driver_register(&drv->drv);
}
EXPORT_SYMBOL_GPL(__gip_register_driver);

void gip_unregister_driver(struct gip_driver *drv)
{
	driver_unregister(&drv->drv);
}
EXPORT_SYMBOL_GPL(gip_unregister_driver);

static int __init gip_bus_init(void)
{
	return bus_register(&gip_bus_type);
}

static void __exit gip_bus_exit(void)
{
	bus_unregister(&gip_bus_type);
}

module_init(gip_bus_init);
module_exit(gip_bus_exit);

MODULE_AUTHOR("Severin von Wnuck <severinvonw@outlook.de>");
MODULE_DESCRIPTION("xone GIP driver");
MODULE_LICENSE("GPL");
