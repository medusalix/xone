// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022 Severin von Wnuck <severinvonw@outlook.de>
 */

#include <linux/module.h>

#include "common.h"

#define GIP_GT_NAME "Mad Catz Rock Band 4 Stratocaster"

enum gip_guitar_button {
	GIP_GT_BTN_MENU = BIT(2),
	GIP_GT_BTN_VIEW = BIT(3),
	GIP_GT_BTN_DPAD_U = BIT(8),
	GIP_GT_BTN_DPAD_D = BIT(9),
	GIP_GT_BTN_DPAD_L = BIT(10),
	GIP_GT_BTN_DPAD_R = BIT(11),
};

enum gip_guitar_fret {
	GIP_GT_FRET_GREEN = BIT(0),
	GIP_GT_FRET_RED = BIT(1),
	GIP_GT_FRET_YELLOW = BIT(2),
	GIP_GT_FRET_BLUE = BIT(3),
	GIP_GT_FRET_ORANGE = BIT(4),
};

struct gip_guitar_pkt_input {
	__le16 buttons;
	u8 tilt;
	u8 whammy;
	u8 slider;
	u8 fret_upper;
	u8 fret_lower;
} __packed;

struct gip_guitar {
	struct gip_client *client;
	struct gip_battery battery;
	struct gip_input input;
};

static int gip_guitar_init_input(struct gip_guitar *guitar)
{
	struct input_dev *dev = guitar->input.dev;
	int err;

	input_set_capability(dev, EV_KEY, BTN_MODE);
	input_set_capability(dev, EV_KEY, BTN_START);
	input_set_capability(dev, EV_KEY, BTN_SELECT);
	input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY1);
	input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY2);
	input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY3);
	input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY4);
	input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY5);
	input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY6);
	input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY7);
	input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY8);
	input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY9);
	input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY10);
	input_set_abs_params(dev, ABS_X, 0, 64, 0, 0);
	input_set_abs_params(dev, ABS_Y, 0, 255, 0, 0);
	input_set_abs_params(dev, ABS_Z, 0, 255, 0, 0);
	input_set_abs_params(dev, ABS_HAT0X, -1, 1, 0, 0);
	input_set_abs_params(dev, ABS_HAT0Y, -1, 1, 0, 0);

	err = input_register_device(dev);
	if (err)
		dev_err(&guitar->client->dev, "%s: register failed: %d\n",
			__func__, err);

	return err;
}

static int gip_guitar_op_battery(struct gip_client *client,
				 enum gip_battery_type type,
				 enum gip_battery_level level)
{
	struct gip_guitar *guitar = dev_get_drvdata(&client->dev);

	gip_report_battery(&guitar->battery, type, level);

	return 0;
}

static int gip_guitar_op_guide_button(struct gip_client *client, bool down)
{
	struct gip_guitar *guitar = dev_get_drvdata(&client->dev);

	input_report_key(guitar->input.dev, BTN_MODE, down);
	input_sync(guitar->input.dev);

	return 0;
}

static int gip_guitar_op_input(struct gip_client *client, void *data, u32 len)
{
	struct gip_guitar *guitar = dev_get_drvdata(&client->dev);
	struct gip_guitar_pkt_input *pkt = data;
	struct input_dev *dev = guitar->input.dev;
	u16 buttons = le16_to_cpu(pkt->buttons);

	if (len < sizeof(*pkt))
		return -EINVAL;

	input_report_key(dev, BTN_START, buttons & GIP_GT_BTN_MENU);
	input_report_key(dev, BTN_SELECT, buttons & GIP_GT_BTN_VIEW);
	input_report_key(dev, BTN_TRIGGER_HAPPY1,
			 pkt->fret_upper & GIP_GT_FRET_GREEN);
	input_report_key(dev, BTN_TRIGGER_HAPPY2,
			 pkt->fret_upper & GIP_GT_FRET_RED);
	input_report_key(dev, BTN_TRIGGER_HAPPY3,
			 pkt->fret_upper & GIP_GT_FRET_YELLOW);
	input_report_key(dev, BTN_TRIGGER_HAPPY4,
			 pkt->fret_upper & GIP_GT_FRET_BLUE);
	input_report_key(dev, BTN_TRIGGER_HAPPY5,
			 pkt->fret_upper & GIP_GT_FRET_ORANGE);
	input_report_key(dev, BTN_TRIGGER_HAPPY6,
			 pkt->fret_lower & GIP_GT_FRET_GREEN);
	input_report_key(dev, BTN_TRIGGER_HAPPY7,
			 pkt->fret_lower & GIP_GT_FRET_RED);
	input_report_key(dev, BTN_TRIGGER_HAPPY8,
			 pkt->fret_lower & GIP_GT_FRET_YELLOW);
	input_report_key(dev, BTN_TRIGGER_HAPPY9,
			 pkt->fret_lower & GIP_GT_FRET_BLUE);
	input_report_key(dev, BTN_TRIGGER_HAPPY10,
			 pkt->fret_lower & GIP_GT_FRET_ORANGE);
	input_report_abs(dev, ABS_X, pkt->slider);
	input_report_abs(dev, ABS_Y, pkt->whammy);
	input_report_abs(dev, ABS_Z, pkt->tilt);
	input_report_abs(dev, ABS_HAT0X, !!(buttons & GIP_GT_BTN_DPAD_R) -
					 !!(buttons & GIP_GT_BTN_DPAD_L));
	input_report_abs(dev, ABS_HAT0Y, !!(buttons & GIP_GT_BTN_DPAD_D) -
					 !!(buttons & GIP_GT_BTN_DPAD_U));
	input_sync(dev);

	return 0;
}

static int gip_guitar_probe(struct gip_client *client)
{
	struct gip_guitar *guitar;
	int err;

	guitar = devm_kzalloc(&client->dev, sizeof(*guitar), GFP_KERNEL);
	if (!guitar)
		return -ENOMEM;

	guitar->client = client;

	err = gip_set_power_mode(client, GIP_PWR_ON);
	if (err)
		return err;

	err = gip_init_battery(&guitar->battery, client, GIP_GT_NAME);
	if (err)
		return err;

	err = gip_complete_authentication(client);
	if (err)
		return err;

	err = gip_init_input(&guitar->input, client, GIP_GT_NAME);
	if (err)
		return err;

	err = gip_guitar_init_input(guitar);
	if (err)
		return err;

	dev_set_drvdata(&client->dev, guitar);

	return 0;
}

static struct gip_driver gip_guitar_driver = {
	.name = "xone-gip-guitar",
	.class = "MadCatz.Xbox.Guitar.Stratocaster",
	.ops = {
		.battery = gip_guitar_op_battery,
		.guide_button = gip_guitar_op_guide_button,
		.input = gip_guitar_op_input,
	},
	.probe = gip_guitar_probe,
};
module_gip_driver(gip_guitar_driver);

MODULE_ALIAS("gip:MadCatz.Xbox.Guitar.Stratocaster");
MODULE_AUTHOR("Severin von Wnuck <severinvonw@outlook.de>");
MODULE_DESCRIPTION("xone GIP guitar driver");
MODULE_VERSION("#VERSION#");
MODULE_LICENSE("GPL");
