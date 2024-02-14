/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2021 Severin von Wnuck-Lipinski <severinvonw@outlook.de>
 */

#pragma once

#include "mt76_defs.h"

#define XONE_MT_EP_IN_CMD 0x05
#define XONE_MT_EP_IN_WLAN 0x04
#define XONE_MT_EP_OUT 0x04

#define XONE_MT_NUM_CHANNELS 12

/* 802.11 frame subtype: reserved */
#define XONE_MT_WLAN_RESERVED 0x70

enum xone_mt76_led_mode {
	XONE_MT_LED_BLINK = 0x00,
	XONE_MT_LED_ON = 0x01,
	XONE_MT_LED_OFF = 0x02,
};

enum xone_mt76_event {
	XONE_MT_EVT_BUTTON = 0x04,
	XONE_MT_EVT_CHANNELS = 0x0a,
	XONE_MT_EVT_PACKET_RX = 0x0c,
	XONE_MT_EVT_COREDUMP = 0x0d,
	XONE_MT_EVT_CLIENT_LOST = 0x0e,
};

enum xone_mt76_client_command {
	XONE_MT_CLIENT_PAIR_REQ = 0x01,
	XONE_MT_CLIENT_PAIR_RESP = 0x02,
	XONE_MT_CLIENT_CHANGE_CHAN_REQ = 0x03,
	XONE_MT_CLIENT_CHANGE_CHAN_RESP = 0x04,
	XONE_MT_CLIENT_STATISTICS_REQ = 0x05,
	XONE_MT_CLIENT_STATISTICS_RESP = 0x06,
	XONE_MT_CLIENT_SCAN_CHAN_REQ = 0x07,
	XONE_MT_CLIENT_SCAN_CHAN_RESP = 0x08,
	XONE_MT_CLIENT_ENABLE_ENCRYPTION = 0x10,
};

struct xone_mt76_channel {
	u8 index;
	u8 band;
	enum mt76_phy_bandwidth bandwidth;
	enum mt76_cal_channel_group group;
	bool scan;
	u8 power;
};

struct xone_mt76 {
	struct device *dev;
	struct usb_device *udev;

	__le32 control_data;
	u8 address[ETH_ALEN];

	struct xone_mt76_channel channels[XONE_MT_NUM_CHANNELS];
	struct xone_mt76_channel *channel;
};

struct sk_buff *xone_mt76_alloc_message(int len, gfp_t gfp);
void xone_mt76_prep_command(struct sk_buff *skb, enum mt76_mcu_cmd cmd);

int xone_mt76_set_led_mode(struct xone_mt76 *mt, enum xone_mt76_led_mode mode);
int xone_mt76_load_firmware(struct xone_mt76 *mt, const char *name);
int xone_mt76_init_radio(struct xone_mt76 *mt);
int xone_mt76_suspend_radio(struct xone_mt76 *mt);
int xone_mt76_resume_radio(struct xone_mt76 *mt);
int xone_mt76_set_pairing(struct xone_mt76 *mt, bool enable);

int xone_mt76_pair_client(struct xone_mt76 *mt, u8 *addr);
int xone_mt76_associate_client(struct xone_mt76 *mt, u8 wcid, u8 *addr);
int xone_mt76_send_client_command(struct xone_mt76 *mt, u8 wcid, u8 *addr,
				  enum xone_mt76_client_command cmd,
				  u8 *data, int len);
int xone_mt76_set_client_key(struct xone_mt76 *mt, u8 wcid, u8 *key, int len);
int xone_mt76_remove_client(struct xone_mt76 *mt, u8 wcid);
