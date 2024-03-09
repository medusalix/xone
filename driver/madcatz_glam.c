// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2024 Severin von Wnuck-Lipinski <severinvonw@outlook.de>
 */

#include <linux/module.h>

#include "common.h"
#include "../auth/auth.h"

#define GIP_GL_NAME "Mad Catz Rock Band 4 Drum Kit"

enum gip_glam_button {
	GIP_GL_BTN_MENU = BIT(2),
	GIP_GL_BTN_VIEW = BIT(3),
	GIP_GL_BTN_A = BIT(4),
	GIP_GL_BTN_B = BIT(5),
	/* swapped X and Y buttons */
	GIP_GL_BTN_X = BIT(7),
	GIP_GL_BTN_Y = BIT(6),
	GIP_GL_BTN_DPAD_U = BIT(8),
	GIP_GL_BTN_DPAD_D = BIT(9),
	GIP_GL_BTN_DPAD_L = BIT(10),
	GIP_GL_BTN_DPAD_R = BIT(11),
	GIP_GL_BTN_KICK_1 = BIT(12),
	GIP_GL_BTN_KICK_2 = BIT(13),
};

enum gip_glam_pad {
	GIP_GL_PAD_YELLOW = BIT(0) | BIT(1) | BIT(2),
	GIP_GL_PAD_RED = BIT(4) | BIT(5) | BIT(6),
	GIP_GL_PAD_GREEN = BIT(8) | BIT(9) | BIT(10),
	GIP_GL_PAD_BLUE = BIT(12) | BIT(13) | BIT(14),
};

enum gip_glam_cymbal {
	GIP_GL_CBL_BLUE = BIT(0) | BIT(1) | BIT(2),
	GIP_GL_CBL_YELLOW = BIT(4) | BIT(5) | BIT(6),
	GIP_GL_CBL_GREEN = BIT(12) | BIT(13) | BIT(14),
};

struct gip_glam_pkt_input {
	__le16 buttons;
	__le16 pads;
	__le16 cymbals;
} __packed;

struct gip_glam {
	struct gip_client *client;
	struct gip_battery battery;
	struct gip_auth auth;
	struct gip_input input;
};

static int gip_glam_init_input(struct gip_glam *glam)
{
	struct input_dev *dev = glam->input.dev;
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
		dev_err(&glam->client->dev, "%s: register failed: %d\n",
			__func__, err);

	return err;
}

static int gip_glam_op_battery(struct gip_client *client,
			       enum gip_battery_type type,
			       enum gip_battery_level level)
{
	struct gip_glam *glam = dev_get_drvdata(&client->dev);

	gip_report_battery(&glam->battery, type, level);

	return 0;
}

static int gip_glam_op_authenticate(struct gip_client *client,
				    void *data, u32 len)
{
	struct gip_glam *glam = dev_get_drvdata(&client->dev);

	return gip_auth_process_pkt(&glam->auth, data, len);
}

static int gip_glam_op_guide_button(struct gip_client *client, bool down)
{
	struct gip_glam *glam = dev_get_drvdata(&client->dev);

	input_report_key(glam->input.dev, BTN_MODE, down);
	input_sync(glam->input.dev);

	return 0;
}

static int gip_glam_op_input(struct gip_client *client, void *data, u32 len)
{
	struct gip_glam *glam = dev_get_drvdata(&client->dev);
	struct gip_glam_pkt_input *pkt = data;
	struct input_dev *dev = glam->input.dev;
	u16 buttons = le16_to_cpu(pkt->buttons);
	u16 pads = le16_to_cpu(pkt->pads);
	u16 cymbals = le16_to_cpu(pkt->cymbals);

	if (len < sizeof(*pkt))
		return -EINVAL;

	input_report_key(dev, BTN_START, buttons & GIP_GL_BTN_MENU);
	input_report_key(dev, BTN_SELECT, buttons & GIP_GL_BTN_VIEW);
	input_report_key(dev, BTN_A, buttons & GIP_GL_BTN_A);
	input_report_key(dev, BTN_B, buttons & GIP_GL_BTN_B);
	input_report_key(dev, BTN_X, buttons & GIP_GL_BTN_X);
	input_report_key(dev, BTN_Y, buttons & GIP_GL_BTN_Y);
	input_report_key(dev, BTN_TRIGGER_HAPPY1, buttons & GIP_GL_BTN_KICK_1);
	input_report_key(dev, BTN_TRIGGER_HAPPY2, buttons & GIP_GL_BTN_KICK_2);
	input_report_key(dev, BTN_TRIGGER_HAPPY3, pads & GIP_GL_PAD_RED);
	input_report_key(dev, BTN_TRIGGER_HAPPY4, pads & GIP_GL_PAD_YELLOW);
	input_report_key(dev, BTN_TRIGGER_HAPPY5, pads & GIP_GL_PAD_BLUE);
	input_report_key(dev, BTN_TRIGGER_HAPPY6, pads & GIP_GL_PAD_GREEN);
	input_report_key(dev, BTN_TRIGGER_HAPPY7, cymbals & GIP_GL_CBL_YELLOW);
	input_report_key(dev, BTN_TRIGGER_HAPPY8, cymbals & GIP_GL_CBL_BLUE);
	input_report_key(dev, BTN_TRIGGER_HAPPY9, cymbals & GIP_GL_CBL_GREEN);
	input_report_abs(dev, ABS_HAT0X, !!(buttons & GIP_GL_BTN_DPAD_R) -
					 !!(buttons & GIP_GL_BTN_DPAD_L));
	input_report_abs(dev, ABS_HAT0Y, !!(buttons & GIP_GL_BTN_DPAD_D) -
					 !!(buttons & GIP_GL_BTN_DPAD_U));
	input_sync(dev);

	return 0;
}

static int gip_glam_probe(struct gip_client *client)
{
	struct gip_glam *glam;
	int err;

	glam = devm_kzalloc(&client->dev, sizeof(*glam), GFP_KERNEL);
	if (!glam)
		return -ENOMEM;

	glam->client = client;

	err = gip_set_power_mode(client, GIP_PWR_ON);
	if (err)
		return err;

	err = gip_init_battery(&glam->battery, client, GIP_GL_NAME);
	if (err)
		return err;

	err = gip_auth_start_handshake(&glam->auth, client);
	if (err)
		return err;

	err = gip_init_input(&glam->input, client, GIP_GL_NAME);
	if (err)
		return err;

	err = gip_glam_init_input(glam);
	if (err)
		return err;

	dev_set_drvdata(&client->dev, glam);

	return 0;
}

static struct gip_driver gip_glam_driver = {
	.name = "xone-gip-madcatz-glam",
	.class = "MadCatz.Xbox.Drums.Glam",
	.ops = {
		.battery = gip_glam_op_battery,
		.authenticate = gip_glam_op_authenticate,
		.guide_button = gip_glam_op_guide_button,
		.input = gip_glam_op_input,
	},
	.probe = gip_glam_probe,
};
module_gip_driver(gip_glam_driver);

MODULE_ALIAS("gip:MadCatz.Xbox.Drums.Glam");
MODULE_AUTHOR("Severin von Wnuck-Lipinski <severinvonw@outlook.de>");
MODULE_DESCRIPTION("xone GIP Mad Catz Drum Kit driver");
MODULE_VERSION("#VERSION#");
MODULE_LICENSE("GPL");
