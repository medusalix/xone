// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2021 Severin von Wnuck-Lipinski <severinvonw@outlook.de>
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

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
static int gip_client_uevent(struct device *dev, struct kobj_uevent_env *env)
#else
static int gip_client_uevent(const struct device *dev,
			     struct kobj_uevent_env *env)
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
	kfree(client->chunk_buf_out);
	kfree(client->chunk_buf_in);
	kfree(client);
}

static struct device_type gip_client_type = {
	.uevent = gip_client_uevent,
	.release = gip_client_release,
};

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
static int gip_bus_match(struct device *dev, struct device_driver *driver)
#else
static int gip_bus_match(struct device *dev, const struct device_driver *driver)
#endif
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
	int err = 0;

	if (down_interruptible(&client->drv_lock))
		return -EINTR;

	if (!client->drv) {
		err = drv->probe(client);
		if (!err)
			client->drv = drv;
	}

	up(&client->drv_lock);

	return err;
}

static void gip_bus_remove(struct device *dev)
{
	struct gip_client *client = to_gip_client(dev);
	struct gip_driver *drv;

	down(&client->drv_lock);

	drv = client->drv;
	if (drv) {
		client->drv = NULL;
		if (drv->remove)
			drv->remove(client);
	}

	up(&client->drv_lock);
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

	adap->clients_wq = alloc_ordered_workqueue("gip%d", 0, adap->id);
	if (!adap->clients_wq) {
		err = -ENOMEM;
		goto err_remove_ida;
	}

	adap->dev.parent = parent;
	adap->dev.type = &gip_adapter_type;
	adap->dev.bus = &gip_bus_type;
	adap->ops = ops;
	adap->audio_packet_count = audio_pkts;
	dev_set_name(&adap->dev, "gip%d", adap->id);
	spin_lock_init(&adap->send_lock);

	err = device_register(&adap->dev);
	if (err)
		goto err_destroy_queue;

	dev_dbg(&adap->dev, "%s: registered\n", __func__);

	return adap;

err_destroy_queue:
	destroy_workqueue(adap->clients_wq);
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
	flush_workqueue(adap->clients_wq);

	for (i = GIP_MAX_CLIENTS - 1; i >= 0; i--) {
		client = adap->clients[i];
		if (!client || !device_is_registered(&client->dev))
			continue;

		device_unregister(&client->dev);
	}

	ida_simple_remove(&gip_adapter_ida, adap->id);
	destroy_workqueue(adap->clients_wq);

	dev_dbg(&adap->dev, "%s: unregistered\n", __func__);
	device_unregister(&adap->dev);
}
EXPORT_SYMBOL_GPL(gip_destroy_adapter);

static void gip_register_client(struct work_struct *work)
{
	struct gip_client *client = container_of(work, typeof(*client),
						 work_register);
	int err;

	client->dev.parent = &client->adapter->dev;
	client->dev.type = &gip_client_type;
	client->dev.bus = &gip_bus_type;
	sema_init(&client->drv_lock, 1);
	dev_set_name(&client->dev, "gip%d.%u", client->adapter->id, client->id);

	err = device_register(&client->dev);
	if (err)
		dev_err(&client->dev, "%s: register failed: %d\n",
			__func__, err);
	else
		dev_dbg(&client->dev, "%s: registered\n", __func__);
}

static void gip_unregister_client(struct work_struct *work)
{
	struct gip_client *client = container_of(work, typeof(*client),
						 work_unregister);

	if (!device_is_registered(&client->dev))
		return;

	dev_dbg(&client->dev, "%s: unregistered\n", __func__);
	device_unregister(&client->dev);
}

struct gip_client *gip_get_client(struct gip_adapter *adap, u8 id)
{
	struct gip_client *client;

	client = adap->clients[id];
	if (client)
		return client;

	client = kzalloc(sizeof(*client), GFP_ATOMIC);
	if (!client)
		return ERR_PTR(-ENOMEM);

	client->id = id;
	client->adapter = adap;
	sema_init(&client->drv_lock, 1);
	INIT_WORK(&client->work_register, gip_register_client);
	INIT_WORK(&client->work_unregister, gip_unregister_client);

	adap->clients[id] = client;

	dev_dbg(&client->adapter->dev, "%s: initialized client %u\n",
		__func__, id);

	return client;
}

void gip_add_client(struct gip_client *client)
{
	queue_work(client->adapter->clients_wq, &client->work_register);
}

void gip_remove_client(struct gip_client *client)
{
	client->adapter->clients[client->id] = NULL;
	queue_work(client->adapter->clients_wq, &client->work_unregister);
}

void gip_free_client_info(struct gip_client *client)
{
	int i;

	kfree(client->client_commands);
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

	client->client_commands = NULL;
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

MODULE_AUTHOR("Severin von Wnuck-Lipinski <severinvonw@outlook.de>");
MODULE_DESCRIPTION("xone GIP driver");
MODULE_VERSION("#VERSION#");
MODULE_LICENSE("GPL");
