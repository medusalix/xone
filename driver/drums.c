// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022 Severin von Wnuck <severinvonw@outlook.de>
 */

#include <linux/module.h>

#include "common.h"

#define GIP_DM_NAME "Mad Catz Rock Band 4 Drum Kit"

enum gip_drums_button {
	GIP_DM_BTN_MENU = BIT(2),
	GIP_DM_BTN_VIEW = BIT(3),
	GIP_DM_BTN_A = BIT(4),
	GIP_DM_BTN_B = BIT(5),
	GIP_DM_BTN_X = BIT(6),
	GIP_DM_BTN_Y = BIT(7),
	GIP_DM_BTN_DPAD_U = BIT(8),
	GIP_DM_BTN_DPAD_D = BIT(9),
	GIP_DM_BTN_DPAD_L = BIT(10),
	GIP_DM_BTN_DPAD_R = BIT(11),
};

enum gip_drums_tom {
	GIP_DM_TOM_RED = BIT(0),
	GIP_DM_TOM_YELLOW = BIT(1),
	GIP_DM_TOM_BLUE = BIT(2),
	GIP_DM_TOM_GREEN = BIT(3),
	GIP_DM_TOM_ORANGE = BIT(4),
	GIP_DM_TOM_ORANGEOPT = BIT(5),
};

enum gip_drums_cymbal {
	GIP_DM_CBL_YELLOW = BIT(0),
	GIP_DM_CBL_BLUE = BIT(1),
	GIP_DM_CBL_GREEN = BIT(2),
};

struct gip_drums_pkt_input {
	__le16 buttons;
	u8 toms;
	u8 cymbals;
} __packed;

struct gip_drums {
	struct gip_client *client;
	struct gip_battery battery;
	struct gip_input input;
};

static int gip_drums_init_input(struct gip_drums *drums)
{
	struct input_dev *dev = drums->input.dev;
	int err;

	input_set_capability(dev, EV_KEY, BTN_MODE);
	input_set_capability(dev, EV_KEY, BTN_START);
	input_set_capability(dev, EV_KEY, BTN_SELECT);
	input_set_capability(dev, EV_KEY, BTN_A);
	input_set_capability(dev, EV_KEY, BTN_B);
	input_set_capability(dev, EV_KEY, BTN_X);
	input_set_capability(dev, EV_KEY, BTN_Y);
	input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY1);
	input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY2);
	input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY3);
	input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY4);
	input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY5);
	input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY6);
	input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY7);
	input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY8);
	input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY9);
	input_set_abs_params(dev, ABS_HAT0X, -1, 1, 0, 0);
	input_set_abs_params(dev, ABS_HAT0Y, -1, 1, 0, 0);

	err = input_register_device(dev);
	if (err)
		dev_err(&drums->client->dev, "%s: register failed: %d\n",
			__func__, err);

	return err;
}

static int gip_drums_op_battery(struct gip_client *client,
				enum gip_battery_type type,
				enum gip_battery_level level)
{
	struct gip_drums *drums = dev_get_drvdata(&client->dev);

	gip_report_battery(&drums->battery, type, level);

	return 0;
}

static int gip_drums_op_guide_button(struct gip_client *client, bool down)
{
	struct gip_drums *drums = dev_get_drvdata(&client->dev);

	input_report_key(drums->input.dev, BTN_MODE, down);
	input_sync(drums->input.dev);

	return 0;
}

static int gip_drums_op_input(struct gip_client *client, void *data, u32 len)
{
	struct gip_drums *drums = dev_get_drvdata(&client->dev);
	struct gip_drums_pkt_input *pkt = data;
	struct input_dev *dev = drums->input.dev;
	u16 buttons = le16_to_cpu(pkt->buttons);

	if (len < sizeof(*pkt))
		return -EINVAL;

	input_report_key(dev, BTN_START, buttons & GIP_DM_BTN_MENU);
	input_report_key(dev, BTN_SELECT, buttons & GIP_DM_BTN_VIEW);
	input_report_key(dev, BTN_A, buttons & GIP_DM_BTN_A);
	input_report_key(dev, BTN_B, buttons & GIP_DM_BTN_B);
	input_report_key(dev, BTN_X, buttons & GIP_DM_BTN_X);
	input_report_key(dev, BTN_Y, buttons & GIP_DM_BTN_Y);
	input_report_key(dev, BTN_TRIGGER_HAPPY1,
			 pkt->toms & GIP_DM_TOM_RED);
	input_report_key(dev, BTN_TRIGGER_HAPPY2,
			 pkt->toms & GIP_DM_TOM_YELLOW);
	input_report_key(dev, BTN_TRIGGER_HAPPY3,
			 pkt->toms & GIP_DM_TOM_BLUE);
	input_report_key(dev, BTN_TRIGGER_HAPPY4,
			 pkt->toms & GIP_DM_TOM_GREEN);
	input_report_key(dev, BTN_TRIGGER_HAPPY5,
			 pkt->toms & GIP_DM_TOM_ORANGE);
	input_report_key(dev, BTN_TRIGGER_HAPPY6,
			 pkt->toms & GIP_DM_TOM_ORANGEOPT);
	input_report_key(dev, BTN_TRIGGER_HAPPY7,
			 pkt->cymbals & GIP_DM_CBL_YELLOW);
	input_report_key(dev, BTN_TRIGGER_HAPPY8,
			 pkt->cymbals & GIP_DM_CBL_BLUE);
	input_report_key(dev, BTN_TRIGGER_HAPPY9,
			 pkt->cymbals & GIP_DM_CBL_GREEN);
	input_report_abs(dev, ABS_HAT0X, !!(buttons & GIP_DM_BTN_DPAD_R) -
					 !!(buttons & GIP_DM_BTN_DPAD_L));
	input_report_abs(dev, ABS_HAT0Y, !!(buttons & GIP_DM_BTN_DPAD_D) -
					 !!(buttons & GIP_DM_BTN_DPAD_U));
	input_sync(dev);

	return 0;
}

static int gip_drums_probe(struct gip_client *client)
{
	struct gip_drums *drums;
	int err;

	drums = devm_kzalloc(&client->dev, sizeof(*drums), GFP_KERNEL);
	if (!drums)
		return -ENOMEM;

	drums->client = client;

	err = gip_set_power_mode(client, GIP_PWR_ON);
	if (err)
		return err;

	err = gip_init_battery(&drums->battery, client, GIP_DM_NAME);
	if (err)
		return err;

	err = gip_complete_authentication(client);
	if (err)
		return err;

	err = gip_init_input(&drums->input, client, GIP_DM_NAME);
	if (err)
		return err;

	err = gip_drums_init_input(drums);
	if (err)
		return err;

	dev_set_drvdata(&client->dev, drums);

	return 0;
}

static struct gip_driver gip_drums_driver = {
	.name = "xone-gip-drums",
	.class = "MadCatz.Xbox.Drums.Glam",
	.ops = {
		.battery = gip_drums_op_battery,
		.guide_button = gip_drums_op_guide_button,
		.input = gip_drums_op_input,
	},
	.probe = gip_drums_probe,
};
module_gip_driver(gip_drums_driver);

MODULE_ALIAS("gip:MadCatz.Xbox.Drums.Glam");
MODULE_AUTHOR("Severin von Wnuck <severinvonw@outlook.de>");
MODULE_DESCRIPTION("xone GIP drums driver");
MODULE_VERSION("#VERSION#");
MODULE_LICENSE("GPL");
