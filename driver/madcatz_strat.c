// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022 Severin von Wnuck-Lipinski <severinvonw@outlook.de>
 */

#include <linux/module.h>

#include "common.h"
#include "../auth/auth.h"

#define GIP_ST_NAME "Mad Catz Rock Band 4 Stratocaster"

enum gip_strat_button {
	GIP_ST_BTN_MENU = BIT(2),
	GIP_ST_BTN_VIEW = BIT(3),
	GIP_ST_BTN_DPAD_U = BIT(8),
	GIP_ST_BTN_DPAD_D = BIT(9),
	GIP_ST_BTN_DPAD_L = BIT(10),
	GIP_ST_BTN_DPAD_R = BIT(11),
};

enum gip_strat_fret {
	GIP_ST_FRET_GREEN = BIT(0),
	GIP_ST_FRET_RED = BIT(1),
	GIP_ST_FRET_YELLOW = BIT(2),
	GIP_ST_FRET_BLUE = BIT(3),
	GIP_ST_FRET_ORANGE = BIT(4),
};

struct gip_strat_pkt_input {
	__le16 buttons;
	u8 tilt;
	u8 whammy;
	u8 slider;
	u8 fret_upper;
	u8 fret_lower;
} __packed;

struct gip_strat {
	struct gip_client *client;
	struct gip_battery battery;
	struct gip_auth auth;
	struct gip_input input;
};

static int gip_strat_init_input(struct gip_strat *strat)
{
	struct input_dev *dev = strat->input.dev;
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
		dev_err(&strat->client->dev, "%s: register failed: %d\n",
			__func__, err);

	return err;
}

static int gip_strat_op_battery(struct gip_client *client,
				enum gip_battery_type type,
				enum gip_battery_level level)
{
	struct gip_strat *strat = dev_get_drvdata(&client->dev);

	gip_report_battery(&strat->battery, type, level);

	return 0;
}

static int gip_strat_op_authenticate(struct gip_client *client,
				     void *data, u32 len)
{
	struct gip_strat *strat = dev_get_drvdata(&client->dev);

	return gip_auth_process_pkt(&strat->auth, data, len);
}

static int gip_strat_op_guide_button(struct gip_client *client, bool down)
{
	struct gip_strat *strat = dev_get_drvdata(&client->dev);

	input_report_key(strat->input.dev, BTN_MODE, down);
	input_sync(strat->input.dev);

	return 0;
}

static int gip_strat_op_input(struct gip_client *client, void *data, u32 len)
{
	struct gip_strat *strat = dev_get_drvdata(&client->dev);
	struct gip_strat_pkt_input *pkt = data;
	struct input_dev *dev = strat->input.dev;
	u16 buttons;

	if (len < sizeof(*pkt))
		return -EINVAL;

	buttons = le16_to_cpu(pkt->buttons);

	input_report_key(dev, BTN_START, buttons & GIP_ST_BTN_MENU);
	input_report_key(dev, BTN_SELECT, buttons & GIP_ST_BTN_VIEW);
	input_report_key(dev, BTN_TRIGGER_HAPPY1,
			 pkt->fret_upper & GIP_ST_FRET_GREEN);
	input_report_key(dev, BTN_TRIGGER_HAPPY2,
			 pkt->fret_upper & GIP_ST_FRET_RED);
	input_report_key(dev, BTN_TRIGGER_HAPPY3,
			 pkt->fret_upper & GIP_ST_FRET_YELLOW);
	input_report_key(dev, BTN_TRIGGER_HAPPY4,
			 pkt->fret_upper & GIP_ST_FRET_BLUE);
	input_report_key(dev, BTN_TRIGGER_HAPPY5,
			 pkt->fret_upper & GIP_ST_FRET_ORANGE);
	input_report_key(dev, BTN_TRIGGER_HAPPY6,
			 pkt->fret_lower & GIP_ST_FRET_GREEN);
	input_report_key(dev, BTN_TRIGGER_HAPPY7,
			 pkt->fret_lower & GIP_ST_FRET_RED);
	input_report_key(dev, BTN_TRIGGER_HAPPY8,
			 pkt->fret_lower & GIP_ST_FRET_YELLOW);
	input_report_key(dev, BTN_TRIGGER_HAPPY9,
			 pkt->fret_lower & GIP_ST_FRET_BLUE);
	input_report_key(dev, BTN_TRIGGER_HAPPY10,
			 pkt->fret_lower & GIP_ST_FRET_ORANGE);
	input_report_abs(dev, ABS_X, pkt->slider);
	input_report_abs(dev, ABS_Y, pkt->whammy);
	input_report_abs(dev, ABS_Z, pkt->tilt);
	input_report_abs(dev, ABS_HAT0X, !!(buttons & GIP_ST_BTN_DPAD_R) -
					 !!(buttons & GIP_ST_BTN_DPAD_L));
	input_report_abs(dev, ABS_HAT0Y, !!(buttons & GIP_ST_BTN_DPAD_D) -
					 !!(buttons & GIP_ST_BTN_DPAD_U));
	input_sync(dev);

	return 0;
}

static int gip_strat_probe(struct gip_client *client)
{
	struct gip_strat *strat;
	int err;

	strat = devm_kzalloc(&client->dev, sizeof(*strat), GFP_KERNEL);
	if (!strat)
		return -ENOMEM;

	strat->client = client;

	err = gip_set_power_mode(client, GIP_PWR_ON);
	if (err)
		return err;

	err = gip_init_battery(&strat->battery, client, GIP_ST_NAME);
	if (err)
		return err;

	err = gip_auth_start_handshake(&strat->auth, client);
	if (err)
		return err;

	err = gip_init_input(&strat->input, client, GIP_ST_NAME);
	if (err)
		return err;

	err = gip_strat_init_input(strat);
	if (err)
		return err;

	dev_set_drvdata(&client->dev, strat);

	return 0;
}

static struct gip_driver gip_strat_driver = {
	.name = "xone-gip-madcatz-strat",
	.class = "MadCatz.Xbox.Guitar.Stratocaster",
	.ops = {
		.battery = gip_strat_op_battery,
		.authenticate = gip_strat_op_authenticate,
		.guide_button = gip_strat_op_guide_button,
		.input = gip_strat_op_input,
	},
	.probe = gip_strat_probe,
};
module_gip_driver(gip_strat_driver);

MODULE_ALIAS("gip:MadCatz.Xbox.Guitar.Stratocaster");
MODULE_AUTHOR("Severin von Wnuck-Lipinski <severinvonw@outlook.de>");
MODULE_DESCRIPTION("xone GIP Mad Catz Stratocaster driver");
MODULE_VERSION("#VERSION#");
MODULE_LICENSE("GPL");
