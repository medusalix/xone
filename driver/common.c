// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2021 Severin von Wnuck <severinvonw@outlook.de>
 */

#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/leds.h>
#include <linux/input.h>

#include "common.h"

#define GIP_LED_BRIGHTNESS_DEFAULT 20
#define GIP_LED_BRIGHTNESS_MAX 50

static enum power_supply_property gip_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_MODEL_NAME,
};

struct gip_battery {
	const char *name;
	int status;
	int capacity;
};

static int gip_get_battery_prop(struct power_supply *psy,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	struct gip_battery *batt = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = batt->status;
		break;
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		val->intval = batt->capacity;
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_DEVICE;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = batt->name;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

int gip_init_battery(struct gip_common *common)
{
	struct gip_client *client = common->client;
	struct gip_battery *batt;
	struct power_supply_desc *desc;
	struct power_supply_config cfg = {};
	struct power_supply *psy;

	batt = devm_kzalloc(&client->dev, sizeof(*batt), GFP_KERNEL);
	if (!batt)
		return -ENOMEM;

	desc = devm_kzalloc(&client->dev, sizeof(*desc), GFP_KERNEL);
	if (!desc)
		return -ENOMEM;

	batt->name = common->name;
	batt->status = POWER_SUPPLY_STATUS_UNKNOWN;
	batt->capacity = POWER_SUPPLY_CAPACITY_LEVEL_UNKNOWN;

	desc->name = dev_name(&client->dev);
	desc->type = POWER_SUPPLY_TYPE_BATTERY;
	desc->properties = gip_battery_props;
	desc->num_properties = ARRAY_SIZE(gip_battery_props);
	desc->get_property = gip_get_battery_prop;

	cfg.drv_data = batt;

	psy = devm_power_supply_register(&client->dev, desc, &cfg);
	if (IS_ERR(psy)) {
		dev_err(&client->dev, "%s: register failed: %ld\n",
			__func__, PTR_ERR(psy));
		return PTR_ERR(psy);
	}

	power_supply_powers(psy, &client->dev);

	common->power_supply = psy;

	return 0;
}
EXPORT_SYMBOL_GPL(gip_init_battery);

int gip_report_battery(struct gip_common *common,
		       enum gip_battery_type type,
		       enum gip_battery_level level)
{
	struct gip_battery *batt =
		power_supply_get_drvdata(common->power_supply);

	if (type == GIP_BATT_TYPE_NONE)
		batt->status = POWER_SUPPLY_STATUS_NOT_CHARGING;
	else
		batt->status = POWER_SUPPLY_STATUS_DISCHARGING;

	if (type == GIP_BATT_TYPE_NONE)
		batt->capacity = POWER_SUPPLY_CAPACITY_LEVEL_UNKNOWN;
	else if (level == GIP_BATT_LEVEL_LOW)
		batt->capacity = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
	else if (level == GIP_BATT_LEVEL_NORMAL)
		batt->capacity = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
	else if (level == GIP_BATT_LEVEL_HIGH)
		batt->capacity = POWER_SUPPLY_CAPACITY_LEVEL_HIGH;
	else if (level == GIP_BATT_LEVEL_FULL)
		batt->capacity = POWER_SUPPLY_CAPACITY_LEVEL_FULL;

	power_supply_changed(common->power_supply);

	return 0;
}
EXPORT_SYMBOL_GPL(gip_report_battery);

static void gip_led_brightness_set(struct led_classdev *dev,
				   enum led_brightness brightness)
{
	struct gip_client *client = container_of(dev->dev->parent,
						 typeof(*client), dev);
	int err;

	if (dev->flags & LED_UNREGISTERING)
		return;

	dev_dbg(&client->dev, "%s: brightness=%d\n", __func__, brightness);

	err = gip_set_led_mode(client, GIP_LED_ON, brightness);
	if (err)
		dev_err(&client->dev, "%s: set LED mode failed: %d\n",
			__func__, err);
}

int gip_init_led(struct gip_common *common)
{
	struct gip_client *client = common->client;
	struct led_classdev *dev;
	int err;

	/* set default brightness */
	err = gip_set_led_mode(client, GIP_LED_ON, GIP_LED_BRIGHTNESS_DEFAULT);
	if (err) {
		dev_err(&client->dev, "%s: set brightness failed: %d\n",
			__func__, err);
		return err;
	}

	dev = devm_kzalloc(&client->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->name = devm_kasprintf(&client->dev, GFP_KERNEL,
				   "%s:white:status", dev_name(&client->dev));
	if (!dev->name)
		return -ENOMEM;

	dev->brightness = GIP_LED_BRIGHTNESS_DEFAULT;
	dev->max_brightness = GIP_LED_BRIGHTNESS_MAX;
	dev->brightness_set = gip_led_brightness_set;

	err = devm_led_classdev_register(&client->dev, dev);
	if (err) {
		dev_err(&client->dev, "%s: register failed: %d\n",
			__func__, err);
		return err;
	}

	common->led_dev = dev;

	return 0;
}
EXPORT_SYMBOL_GPL(gip_init_led);

int gip_init_input(struct gip_common *common)
{
	struct gip_client *client = common->client;
	struct input_dev *dev;

	dev = devm_input_allocate_device(&client->dev);
	if (!dev)
		return -ENOMEM;

	dev->phys = devm_kasprintf(&client->dev, GFP_KERNEL,
				   "%s/input0", dev_name(&client->dev));
	if (!dev->phys)
		return -ENOMEM;

	dev->name = common->name;
	dev->id.bustype = BUS_VIRTUAL;
	dev->id.vendor = client->hardware.vendor;
	dev->id.product = client->hardware.product;
	dev->id.version = client->hardware.version;
	dev->dev.parent = &client->dev;

	common->input_dev = dev;

	return 0;
}
EXPORT_SYMBOL_GPL(gip_init_input);

MODULE_AUTHOR("Severin von Wnuck <severinvonw@outlook.de>");
MODULE_DESCRIPTION("xone GIP common driver");
MODULE_VERSION("#VERSION#");
MODULE_LICENSE("GPL");
