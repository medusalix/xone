/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2021 Severin von Wnuck <severinvonw@outlook.de>
 */

#pragma once

#include <linux/power_supply.h>
#include <linux/leds.h>
#include <linux/input.h>

#include "../bus/bus.h"

struct gip_battery {
	struct power_supply *supply;
	struct power_supply_desc desc;

	const char *name;
	int status;
	int capacity;
};

struct gip_led {
	struct led_classdev dev;

	struct gip_client *client;
};

struct gip_input {
	struct input_dev *dev;
};

int gip_init_battery(struct gip_battery *batt, struct gip_client *client,
		     const char *name);
int gip_report_battery(struct gip_battery *batt,
		       enum gip_battery_type type,
		       enum gip_battery_level level);

int gip_init_led(struct gip_led *led, struct gip_client *client);

int gip_init_input(struct gip_input *input, struct gip_client *client,
		   const char *name);
