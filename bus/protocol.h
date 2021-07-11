/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2021 Severin von Wnuck <severinvonw@outlook.de>
 */

#pragma once

#include <linux/types.h>

/* time between audio packets in ms */
#define GIP_AUDIO_INTERVAL 8

enum gip_client_state {
	GIP_CL_CONNECTED,
	GIP_CL_ANNOUNCED,
	GIP_CL_IDENTIFIED,
	GIP_CL_DISCONNECTED,
};

enum gip_battery_type {
	GIP_BATT_TYPE_NONE = 0x00,
	GIP_BATT_TYPE_STANDARD = 0x01,
	GIP_BATT_TYPE_KIT = 0x02,
	GIP_BATT_TYPE_UNKNOWN = 0x03,
};

enum gip_battery_level {
	GIP_BATT_LEVEL_LOW = 0x00,
	GIP_BATT_LEVEL_NORMAL = 0x01,
	GIP_BATT_LEVEL_HIGH = 0x02,
	GIP_BATT_LEVEL_FULL = 0x03,
};

enum gip_power_mode {
	GIP_PWR_ON = 0x00,
	GIP_PWR_SLEEP = 0x01,
	GIP_PWR_OFF = 0x04,
	GIP_PWR_RESET = 0x07,
};

enum gip_audio_format {
	GIP_AUD_FORMAT_24KHZ_MONO = 0x09,
	GIP_AUD_FORMAT_48KHZ_STEREO = 0x10,
};

enum gip_audio_format_chat {
	GIP_AUD_FORMAT_CHAT_24KHZ = 0x04,
	GIP_AUD_FORMAT_CHAT_16KHZ = 0x05,
};

enum gip_led_mode {
	GIP_LED_OFF = 0x00,
	GIP_LED_ON = 0x01,
	GIP_LED_BLINK_FAST = 0x02,
	GIP_LED_BLINK_MED = 0x03,
	GIP_LED_BLINK_SLOW = 0x04,
	GIP_LED_FADE_SLOW = 0x08,
	GIP_LED_FADE_FAST = 0x09,
};

struct gip_chunk_buffer {
	bool full;
	int length;
	u8 data[];
};

struct gip_hardware {
	u16 vendor;
	u16 product;
	u16 version;
};

struct gip_info_element {
	u8 count;
	u8 data[];
};

struct gip_audio_config {
	enum gip_audio_format format;

	int channels;
	int sample_rate;

	int buffer_size;
	int fragment_size;
	int packet_size;

	bool valid;
};

struct gip_classes {
	u8 count;
	const char *strings[];
};

struct gip_client;
struct gip_adapter;

int gip_set_power_mode(struct gip_client *client, enum gip_power_mode mode);
int gip_complete_authentication(struct gip_client *client);
int gip_suggest_audio_format(struct gip_client *client,
		enum gip_audio_format in, enum gip_audio_format out);
int gip_fix_audio_volume(struct gip_client *client);
int gip_send_rumble(struct gip_client *client, void *pkt, u8 len);
int gip_set_led_mode(struct gip_client *client,
		enum gip_led_mode mode, u8 brightness);
int gip_send_audio_samples(struct gip_client *client, void *samples);

int gip_enable_audio(struct gip_client *client);
int gip_init_audio_in(struct gip_client *client);
int gip_init_audio_out(struct gip_client *client);
void gip_disable_audio(struct gip_client *client);

int gip_process_buffer(struct gip_adapter *adap, void *data, int len);
