// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2021 Severin von Wnuck <severinvonw@outlook.de>
 */

#include <linux/module.h>
#include <linux/uuid.h>
#include <linux/timer.h>
#include <linux/input.h>

#include "common.h"

#define GIP_GP_NAME "Microsoft X-Box One pad"

/* vendor/product ID for the elite controller series 2 */
#define GIP_GP_VID_MICROSOFT 0x045e
#define GIP_GP_PID_ELITE2 0x0b00

#define GIP_GP_RUMBLE_DELAY msecs_to_jiffies(10)
#define GIP_GP_RUMBLE_MAX 100

static const guid_t gip_gamepad_guid_middle_button =
	GUID_INIT(0xecddd2fe, 0xd387, 0x4294,
		  0xbd, 0x96, 0x1a, 0x71, 0x2e, 0x3d, 0xc7, 0x7d);

enum gip_gamepad_button {
	GIP_GP_BTN_MENU = BIT(2),
	GIP_GP_BTN_VIEW = BIT(3),
	GIP_GP_BTN_A = BIT(4),
	GIP_GP_BTN_B = BIT(5),
	GIP_GP_BTN_X = BIT(6),
	GIP_GP_BTN_Y = BIT(7),
	GIP_GP_BTN_DPAD_U = BIT(8),
	GIP_GP_BTN_DPAD_D = BIT(9),
	GIP_GP_BTN_DPAD_L = BIT(10),
	GIP_GP_BTN_DPAD_R = BIT(11),
	GIP_GP_BTN_BUMPER_L = BIT(12),
	GIP_GP_BTN_BUMPER_R = BIT(13),
	GIP_GP_BTN_STICK_L = BIT(14),
	GIP_GP_BTN_STICK_R = BIT(15),
};

enum gip_gamepad_motor {
	GIP_GP_MOTOR_R = BIT(0),
	GIP_GP_MOTOR_L = BIT(1),
	GIP_GP_MOTOR_RT = BIT(2),
	GIP_GP_MOTOR_LT = BIT(3),
};

struct gip_gamepad_pkt_input {
	__le16 buttons;
	__le16 trigger_left;
	__le16 trigger_right;
	__le16 stick_left_x;
	__le16 stick_left_y;
	__le16 stick_right_x;
	__le16 stick_right_y;
} __packed;

struct gip_gamepad_pkt_series_xs {
	u8 unknown[4];
	u8 share_button;
} __packed;

struct gip_gamepad_pkt_rumble {
	u8 unknown;
	u8 motors;
	u8 left_trigger;
	u8 right_trigger;
	u8 left;
	u8 right;
	u8 duration;
	u8 delay;
	u8 repeat;
} __packed;

struct gip_gamepad {
	struct gip_client *client;
	struct gip_battery battery;
	struct gip_led led;
	struct gip_input input;

	bool series_xs;

	struct gip_gamepad_rumble {
		/* serializes access to rumble packet */
		spinlock_t lock;
		bool queued;
		unsigned long last;
		struct timer_list timer;
		struct gip_gamepad_pkt_rumble pkt;
	} rumble;
};

static void gip_gamepad_send_rumble(struct timer_list *timer)
{
	struct gip_gamepad_rumble *rumble = from_timer(rumble, timer, timer);
	struct gip_gamepad *gamepad = container_of(rumble, typeof(*gamepad),
						   rumble);
	unsigned long flags;

	spin_lock_irqsave(&rumble->lock, flags);

	gip_send_rumble(gamepad->client, &rumble->pkt, sizeof(rumble->pkt));
	rumble->last = jiffies;

	spin_unlock_irqrestore(&rumble->lock, flags);
}

static int gip_gamepad_queue_rumble(struct input_dev *dev, void *data,
				    struct ff_effect *effect)
{
	struct gip_gamepad_rumble *rumble = input_get_drvdata(dev);
	u32 mag_left = effect->u.rumble.strong_magnitude;
	u32 mag_right = effect->u.rumble.weak_magnitude;
	unsigned long flags;

	if (effect->type != FF_RUMBLE)
		return 0;

	spin_lock_irqsave(&rumble->lock, flags);

	rumble->pkt.motors = GIP_GP_MOTOR_R | GIP_GP_MOTOR_L;
	rumble->pkt.left = (mag_left * GIP_GP_RUMBLE_MAX + S16_MAX) / U16_MAX;
	rumble->pkt.right = (mag_right * GIP_GP_RUMBLE_MAX + S16_MAX) / U16_MAX;
	rumble->pkt.duration = 0xff;
	rumble->pkt.repeat = 0xeb;

	/* delay rumble to work around firmware bug */
	if (!timer_pending(&rumble->timer))
		mod_timer(&rumble->timer, rumble->last + GIP_GP_RUMBLE_DELAY);

	spin_unlock_irqrestore(&rumble->lock, flags);

	return 0;
}

static bool gip_gamepad_is_series_xs(struct gip_client *client)
{
	struct gip_hardware *hw = &client->hardware;
	guid_t *guid;
	int i;

	/* the elite controller also has a middle button */
	if (hw->vendor == GIP_GP_VID_MICROSOFT &&
	    hw->product == GIP_GP_PID_ELITE2)
		return false;

	for (i = 0; i < client->interfaces->count; i++) {
		guid = (guid_t *)client->interfaces->data + i;
		if (guid_equal(guid, &gip_gamepad_guid_middle_button))
			return true;
	}

	return false;
}

static int gip_gamepad_init_input(struct gip_gamepad *gamepad)
{
	struct input_dev *dev = gamepad->input.dev;
	int err;

	gamepad->series_xs = gip_gamepad_is_series_xs(gamepad->client);
	if (gamepad->series_xs)
		input_set_capability(dev, EV_KEY, KEY_RECORD);

	input_set_capability(dev, EV_KEY, BTN_MODE);
	input_set_capability(dev, EV_KEY, BTN_START);
	input_set_capability(dev, EV_KEY, BTN_SELECT);
	input_set_capability(dev, EV_KEY, BTN_A);
	input_set_capability(dev, EV_KEY, BTN_B);
	input_set_capability(dev, EV_KEY, BTN_X);
	input_set_capability(dev, EV_KEY, BTN_Y);
	input_set_capability(dev, EV_KEY, BTN_TL);
	input_set_capability(dev, EV_KEY, BTN_TR);
	input_set_capability(dev, EV_KEY, BTN_THUMBL);
	input_set_capability(dev, EV_KEY, BTN_THUMBR);
	input_set_capability(dev, EV_FF, FF_RUMBLE);
	input_set_abs_params(dev, ABS_X, -32768, 32767, 16, 128);
	input_set_abs_params(dev, ABS_RX, -32768, 32767, 16, 128);
	input_set_abs_params(dev, ABS_Y, -32768, 32767, 16, 128);
	input_set_abs_params(dev, ABS_RY, -32768, 32767, 16, 128);
	input_set_abs_params(dev, ABS_Z, 0, 1023, 0, 0);
	input_set_abs_params(dev, ABS_RZ, 0, 1023, 0, 0);
	input_set_abs_params(dev, ABS_HAT0X, -1, 1, 0, 0);
	input_set_abs_params(dev, ABS_HAT0Y, -1, 1, 0, 0);
	input_set_drvdata(dev, &gamepad->rumble);

	err = input_ff_create_memless(dev, NULL, gip_gamepad_queue_rumble);
	if (err) {
		dev_err(&gamepad->client->dev, "%s: create FF failed: %d\n",
			__func__, err);
		return err;
	}

	err = input_register_device(dev);
	if (err) {
		dev_err(&gamepad->client->dev, "%s: register failed: %d\n",
			__func__, err);
		return err;
	}

	spin_lock_init(&gamepad->rumble.lock);
	timer_setup(&gamepad->rumble.timer, gip_gamepad_send_rumble, 0);

	return 0;
}

static int gip_gamepad_op_battery(struct gip_client *client,
				  enum gip_battery_type type,
				  enum gip_battery_level level)
{
	struct gip_gamepad *gamepad = dev_get_drvdata(&client->dev);

	gip_report_battery(&gamepad->battery, type, level);

	return 0;
}

static int gip_gamepad_op_guide_button(struct gip_client *client, bool pressed)
{
	struct gip_gamepad *gamepad = dev_get_drvdata(&client->dev);

	input_report_key(gamepad->input.dev, BTN_MODE, pressed);
	input_sync(gamepad->input.dev);

	return 0;
}

static int gip_gamepad_op_input(struct gip_client *client, void *data, int len)
{
	struct gip_gamepad *gamepad = dev_get_drvdata(&client->dev);
	struct gip_gamepad_pkt_input *pkt = data;
	struct gip_gamepad_pkt_series_xs *pkt_xs = data + sizeof(*pkt);
	struct input_dev *dev = gamepad->input.dev;
	u16 buttons = le16_to_cpu(pkt->buttons);

	if (len < sizeof(*pkt))
		return -EINVAL;

	if (gamepad->series_xs) {
		if (len < sizeof(*pkt) + sizeof(*pkt_xs))
			return -EINVAL;

		input_report_key(dev, KEY_RECORD, !!pkt_xs->share_button);
	}

	input_report_key(dev, BTN_START, buttons & GIP_GP_BTN_MENU);
	input_report_key(dev, BTN_SELECT, buttons & GIP_GP_BTN_VIEW);
	input_report_key(dev, BTN_A, buttons & GIP_GP_BTN_A);
	input_report_key(dev, BTN_B, buttons & GIP_GP_BTN_B);
	input_report_key(dev, BTN_X, buttons & GIP_GP_BTN_X);
	input_report_key(dev, BTN_Y, buttons & GIP_GP_BTN_Y);
	input_report_key(dev, BTN_TL, buttons & GIP_GP_BTN_BUMPER_L);
	input_report_key(dev, BTN_TR, buttons & GIP_GP_BTN_BUMPER_R);
	input_report_key(dev, BTN_THUMBL, buttons & GIP_GP_BTN_STICK_L);
	input_report_key(dev, BTN_THUMBR, buttons & GIP_GP_BTN_STICK_R);
	input_report_abs(dev, ABS_X, (s16)le16_to_cpu(pkt->stick_left_x));
	input_report_abs(dev, ABS_RX, (s16)le16_to_cpu(pkt->stick_right_x));
	input_report_abs(dev, ABS_Y, ~(s16)le16_to_cpu(pkt->stick_left_y));
	input_report_abs(dev, ABS_RY, ~(s16)le16_to_cpu(pkt->stick_right_y));
	input_report_abs(dev, ABS_Z, le16_to_cpu(pkt->trigger_left));
	input_report_abs(dev, ABS_RZ, le16_to_cpu(pkt->trigger_right));
	input_report_abs(dev, ABS_HAT0X, !!(buttons & GIP_GP_BTN_DPAD_R) -
					 !!(buttons & GIP_GP_BTN_DPAD_L));
	input_report_abs(dev, ABS_HAT0Y, !!(buttons & GIP_GP_BTN_DPAD_D) -
					 !!(buttons & GIP_GP_BTN_DPAD_U));
	input_sync(dev);

	return 0;
}

static int gip_gamepad_probe(struct gip_client *client)
{
	struct gip_gamepad *gamepad;
	int err;

	gamepad = devm_kzalloc(&client->dev, sizeof(*gamepad), GFP_KERNEL);
	if (!gamepad)
		return -ENOMEM;

	gamepad->client = client;

	err = gip_init_input(&gamepad->input, client, GIP_GP_NAME);
	if (err)
		return err;

	err = gip_gamepad_init_input(gamepad);
	if (err)
		return err;

	err = gip_init_battery(&gamepad->battery, client, GIP_GP_NAME);
	if (err)
		return err;

	err = gip_set_power_mode(client, GIP_PWR_ON);
	if (err)
		return err;

	err = gip_init_led(&gamepad->led, client);
	if (err)
		return err;

	err = gip_complete_authentication(client);
	if (err)
		return err;

	dev_set_drvdata(&client->dev, gamepad);

	return 0;
}

static void gip_gamepad_remove(struct gip_client *client)
{
	struct gip_gamepad *gamepad = dev_get_drvdata(&client->dev);

	del_timer_sync(&gamepad->rumble.timer);
	dev_set_drvdata(&client->dev, NULL);
}

static struct gip_driver gip_gamepad_driver = {
	.name = "xone-gip-gamepad",
	.class = "Windows.Xbox.Input.Gamepad",
	.ops = {
		.battery = gip_gamepad_op_battery,
		.guide_button = gip_gamepad_op_guide_button,
		.input = gip_gamepad_op_input,
	},
	.probe = gip_gamepad_probe,
	.remove = gip_gamepad_remove,
};
module_gip_driver(gip_gamepad_driver);

MODULE_ALIAS("gip:Windows.Xbox.Input.Gamepad");
MODULE_AUTHOR("Severin von Wnuck <severinvonw@outlook.de>");
MODULE_DESCRIPTION("xone GIP gamepad driver");
MODULE_VERSION("#VERSION#");
MODULE_LICENSE("GPL");
