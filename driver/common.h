/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2021 Severin von Wnuck <severinvonw@outlook.de>
 */

#pragma once

#include "../bus/bus.h"

struct power_supply;
struct led_classdev;
struct input_dev;

struct gip_common {
	struct gip_client *client;
	const char *name;

	struct power_supply *power_supply;
	struct led_classdev *led_dev;
	struct input_dev *input_dev;
};

int gip_init_battery(struct gip_common *common);
int gip_report_battery(struct gip_common *common,
		       enum gip_battery_type type,
		       enum gip_battery_level level);
int gip_init_led(struct gip_common *common);
int gip_init_input(struct gip_common *common);
