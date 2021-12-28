// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2021 Severin von Wnuck <severinvonw@outlook.de>
 */

#include <linux/slab.h>
#include <linux/bitfield.h>
#include <linux/uuid.h>

#include "bus.h"

/* vendor/product ID for the chat headset */
#define GIP_VID_MICROSOFT 0x045e
#define GIP_PID_CHAT_HEADSET 0x0111

#define GIP_HDR_CLIENT_ID GENMASK(3, 0)
#define GIP_HDR_LENGTH GENMASK(6, 0)
#define GIP_HDR_EXTENDED BIT(7)

#define GIP_BATT_LEVEL GENMASK(1, 0)
#define GIP_BATT_TYPE GENMASK(3, 2)
#define GIP_STATUS_CONNECTED BIT(7)

enum gip_command_internal {
	GIP_CMD_ACKNOWLEDGE = 0x01,
	GIP_CMD_ANNOUNCE = 0x02,
	GIP_CMD_STATUS = 0x03,
	GIP_CMD_IDENTIFY = 0x04,
	GIP_CMD_POWER = 0x05,
	GIP_CMD_AUTHENTICATE = 0x06,
	GIP_CMD_GUIDE_BUTTON = 0x07,
	GIP_CMD_AUDIO_CONTROL = 0x08,
	GIP_CMD_LED = 0x0a,
	GIP_CMD_HID_REPORT = 0x0b,
	GIP_CMD_FIRMWARE = 0x0c,
	GIP_CMD_SERIAL_NUMBER = 0x1e,
	GIP_CMD_AUDIO_SAMPLES = 0x60,
};

enum gip_command_external {
	GIP_CMD_RUMBLE = 0x09,
	GIP_CMD_INPUT = 0x20,
};

enum gip_option {
	GIP_OPT_ACKNOWLEDGE = BIT(4),
	GIP_OPT_INTERNAL = BIT(5),
	GIP_OPT_CHUNK_START = BIT(6),
	GIP_OPT_CHUNK = BIT(7),
};

enum gip_audio_control {
	GIP_AUD_CTRL_VOLUME_CHAT = 0x00,
	GIP_AUD_CTRL_FORMAT_CHAT = 0x01,
	GIP_AUD_CTRL_FORMAT = 0x02,
	GIP_AUD_CTRL_VOLUME = 0x03,
};

enum gip_audio_volume_mute {
	GIP_AUD_VOLUME_UNMUTED = 0x04,
	GIP_AUD_VOLUME_MIC_MUTED = 0x05,
};

struct gip_header {
	u8 command;
	u8 options;
	u8 sequence;
	u8 length;
} __packed;

struct gip_chunk_header {
	u8 offset_extra;
	u8 offset;
} __packed;

struct gip_pkt_acknowledge {
	u8 unknown;
	struct gip_header inner;
	u8 padding[2];
	__le16 remaining;
} __packed;

struct gip_pkt_announce {
	u8 address[6];
	__le16 unknown;
	__le16 vendor_id;
	__le16 product_id;
	struct gip_version {
		__le16 major;
		__le16 minor;
		__le16 build;
		__le16 revision;
	} __packed fw_version, hw_version;
} __packed;

struct gip_pkt_status {
	u8 status;
	u8 unknown[3];
} __packed;

struct gip_pkt_identify {
	u8 unknown[16];
	__le16 external_commands_offset;
	__le16 unknown_offset;
	__le16 audio_formats_offset;
	__le16 capabilities_out_offset;
	__le16 capabilities_in_offset;
	__le16 classes_offset;
	__le16 interfaces_offset;
	__le16 hid_descriptor_offset;
} __packed;

struct gip_pkt_power {
	u8 mode;
} __packed;

struct gip_pkt_authenticate {
	u8 unknown1;
	u8 unknown2;
} __packed;

struct gip_pkt_guide_button {
	u8 pressed;
	u8 unknown;
} __packed;

struct gip_pkt_audio_control {
	u8 subcommand;
} __packed;

struct gip_pkt_audio_volume_chat {
	struct gip_pkt_audio_control control;
	u8 mute;
	u8 gain_out;
	u8 out;
	u8 in;
} __packed;

struct gip_pkt_audio_format_chat {
	struct gip_pkt_audio_control control;
	u8 in_out;
} __packed;

struct gip_pkt_audio_format {
	struct gip_pkt_audio_control control;
	u8 in;
	u8 out;
} __packed;

struct gip_pkt_audio_volume {
	struct gip_pkt_audio_control control;
	u8 mute;
	u8 out;
	u8 unknown1;
	u8 in;
	u8 unknown2;
	u8 unknown3[2];
} __packed;

struct gip_pkt_led {
	u8 unknown;
	u8 mode;
	u8 brightness;
} __packed;

struct gip_pkt_serial_number {
	u8 unknown[2];
	char serial[14];
} __packed;

struct gip_pkt_audio_header {
	u8 length_extra;
	u8 unknown;
} __packed;

struct gip_command_descriptor {
	u8 marker;
	u8 unknown1;
	u8 command;
	u8 length;
	u8 unknown2[3];
	u8 options;
	u8 unknown3[15];
} __packed;

struct gip_chunk {
	int offset;
	void *data;
	int length;
};

static int gip_send_pkt(struct gip_client *client,
			struct gip_header *hdr, void *data, int len)
{
	struct gip_adapter *adap = client->adapter;
	struct gip_adapter_buffer buf = {};
	int err;
	unsigned long flags;

	buf.type = GIP_BUF_DATA;

	spin_lock_irqsave(&adap->send_lock, flags);

	/* sequence number is always greater than zero */
	while (!hdr->sequence)
		hdr->sequence = adap->data_sequence++;

	err = adap->ops->get_buffer(adap, &buf);
	if (err) {
		dev_err(&client->dev, "%s: get buffer failed: %d\n",
			__func__, err);
		goto err_unlock;
	}

	memcpy(buf.data, hdr, sizeof(*hdr));
	if (data)
		memcpy(buf.data + sizeof(*hdr), data, len);

	/* set actual length */
	buf.length = sizeof(*hdr) + len;

	/* always fails on adapter removal */
	err = adap->ops->submit_buffer(adap, &buf);
	if (err)
		dev_dbg(&client->dev, "%s: submit buffer failed: %d\n",
			__func__, err);

err_unlock:
	spin_unlock_irqrestore(&adap->send_lock, flags);

	return err;
}

bool gip_is_accessory(struct gip_client *client)
{
	return client->id > 0;
}
EXPORT_SYMBOL_GPL(gip_is_accessory);

static int gip_acknowledge_pkt(struct gip_client *client,
			       struct gip_header *ack, u16 len, u16 remaining)
{
	struct gip_header hdr = {};
	struct gip_pkt_acknowledge pkt = {};

	hdr.command = GIP_CMD_ACKNOWLEDGE;
	hdr.options = client->id | GIP_OPT_INTERNAL;
	hdr.sequence = ack->sequence;
	hdr.length = sizeof(pkt);

	pkt.inner.command = ack->command;
	pkt.inner.options = client->id | GIP_OPT_INTERNAL;
	pkt.inner.sequence = len;
	pkt.inner.length = len >> 8;

	/* only required for the start chunk */
	pkt.remaining = cpu_to_le16(remaining);

	return gip_send_pkt(client, &hdr, &pkt, sizeof(pkt));
}

static int gip_request_identification(struct gip_client *client)
{
	struct gip_header hdr = {};

	hdr.command = GIP_CMD_IDENTIFY;
	hdr.options = client->id | GIP_OPT_INTERNAL;

	return gip_send_pkt(client, &hdr, NULL, 0);
}

int gip_set_power_mode(struct gip_client *client, enum gip_power_mode mode)
{
	struct gip_header hdr = {};
	struct gip_pkt_power pkt = {};

	hdr.command = GIP_CMD_POWER;
	hdr.options = client->id | GIP_OPT_INTERNAL;
	hdr.length = sizeof(pkt);

	pkt.mode = mode;

	return gip_send_pkt(client, &hdr, &pkt, sizeof(pkt));
}
EXPORT_SYMBOL_GPL(gip_set_power_mode);

int gip_complete_authentication(struct gip_client *client)
{
	struct gip_header hdr = {};
	struct gip_pkt_authenticate pkt = {};

	hdr.command = GIP_CMD_AUTHENTICATE;
	hdr.options = client->id | GIP_OPT_INTERNAL;
	hdr.length = sizeof(pkt);

	pkt.unknown1 = 0x01;

	return gip_send_pkt(client, &hdr, &pkt, sizeof(pkt));
}
EXPORT_SYMBOL_GPL(gip_complete_authentication);

static int gip_set_audio_format_chat(struct gip_client *client,
				     enum gip_audio_format_chat in_out)
{
	struct gip_header hdr = {};
	struct gip_pkt_audio_format_chat pkt = {};

	hdr.command = GIP_CMD_AUDIO_CONTROL;
	hdr.options = client->id | GIP_OPT_INTERNAL;
	hdr.length = sizeof(pkt);

	pkt.control.subcommand = GIP_AUD_CTRL_FORMAT_CHAT;
	pkt.in_out = in_out;

	return gip_send_pkt(client, &hdr, &pkt, sizeof(pkt));
}

static int gip_set_audio_format(struct gip_client *client,
				enum gip_audio_format in,
				enum gip_audio_format out)
{
	struct gip_header hdr = {};
	struct gip_pkt_audio_format pkt = {};

	hdr.command = GIP_CMD_AUDIO_CONTROL;
	hdr.options = client->id | GIP_OPT_INTERNAL;
	hdr.length = sizeof(pkt);

	pkt.control.subcommand = GIP_AUD_CTRL_FORMAT;
	pkt.in = in;
	pkt.out = out;

	return gip_send_pkt(client, &hdr, &pkt, sizeof(pkt));
}

int gip_suggest_audio_format(struct gip_client *client,
			     enum gip_audio_format in,
			     enum gip_audio_format out)
{
	struct gip_hardware *hw = &client->hardware;
	int err;

	/* special handling for the chat headset */
	if (hw->vendor == GIP_VID_MICROSOFT &&
	    hw->product == GIP_PID_CHAT_HEADSET)
		err = gip_set_audio_format_chat(client,
						GIP_AUD_FORMAT_CHAT_24KHZ);
	else
		err = gip_set_audio_format(client, in, out);

	if (err) {
		dev_err(&client->dev, "%s: set format failed: %d\n",
			__func__, err);
		return err;
	}

	client->audio_config_in.format = in;
	client->audio_config_out.format = out;

	return 0;
}
EXPORT_SYMBOL_GPL(gip_suggest_audio_format);

static int gip_set_audio_volume(struct gip_client *client, u8 in, u8 out)
{
	struct gip_header hdr = {};
	struct gip_pkt_audio_volume pkt = {};

	hdr.command = GIP_CMD_AUDIO_CONTROL;
	hdr.options = client->id | GIP_OPT_INTERNAL;
	hdr.length = sizeof(pkt);

	pkt.control.subcommand = GIP_AUD_CTRL_VOLUME;
	pkt.mute = GIP_AUD_VOLUME_UNMUTED;
	pkt.out = out;
	pkt.in = in;

	return gip_send_pkt(client, &hdr, &pkt, sizeof(pkt));
}

int gip_fix_audio_volume(struct gip_client *client)
{
	struct gip_hardware *hw = &client->hardware;

	/* chat headsets have buttons to adjust the hardware volume */
	if (hw->vendor == GIP_VID_MICROSOFT &&
	    hw->product == GIP_PID_CHAT_HEADSET)
		return 0;

	/* set hardware volume to maximum */
	return gip_set_audio_volume(client, 100, 100);
}
EXPORT_SYMBOL_GPL(gip_fix_audio_volume);

int gip_send_rumble(struct gip_client *client, void *pkt, u8 len)
{
	struct gip_header hdr = {};

	hdr.command = GIP_CMD_RUMBLE;
	hdr.options = client->id;
	hdr.length = len;

	return gip_send_pkt(client, &hdr, pkt, len);
}
EXPORT_SYMBOL_GPL(gip_send_rumble);

int gip_set_led_mode(struct gip_client *client,
		     enum gip_led_mode mode, u8 brightness)
{
	struct gip_header hdr = {};
	struct gip_pkt_led pkt = {};

	hdr.command = GIP_CMD_LED;
	hdr.options = client->id | GIP_OPT_INTERNAL;
	hdr.length = sizeof(pkt);

	pkt.mode = mode;
	pkt.brightness = brightness;

	return gip_send_pkt(client, &hdr, &pkt, sizeof(pkt));
}
EXPORT_SYMBOL_GPL(gip_set_led_mode);

static void gip_copy_audio_samples(struct gip_client *client,
				   void *samples, void *buf)
{
	struct gip_audio_config *cfg = &client->audio_config_out;
	struct gip_header hdr = {};
	struct gip_pkt_audio_header pkt = {};
	void *src, *dest;
	int i;

	/* packet length does not include audio header size */
	hdr.command = GIP_CMD_AUDIO_SAMPLES;
	hdr.options = client->id | GIP_OPT_INTERNAL;
	hdr.length = cfg->fragment_size;

	if (cfg->fragment_size > GIP_HDR_LENGTH) {
		hdr.length |= GIP_HDR_EXTENDED;
		pkt.length_extra = GIP_HDR_EXTENDED | (cfg->fragment_size >> 7);
	}

	for (i = 0; i < client->adapter->audio_packet_count; i++) {
		src = samples + i * cfg->fragment_size;
		dest = buf + i * cfg->packet_size;

		/* sequence number is always greater than zero */
		do {
			hdr.sequence = client->adapter->audio_sequence++;
		} while (!hdr.sequence);

		memcpy(dest, &hdr, sizeof(hdr));

		if (cfg->fragment_size > GIP_HDR_LENGTH) {
			memcpy(dest + sizeof(hdr), &pkt, sizeof(pkt));
			memcpy(dest + sizeof(hdr) + sizeof(pkt), src,
			       cfg->fragment_size);
		} else {
			memcpy(dest + sizeof(hdr), src, cfg->fragment_size);
		}
	}
}

int gip_send_audio_samples(struct gip_client *client, void *samples)
{
	struct gip_adapter *adap = client->adapter;
	struct gip_adapter_buffer buf = {};
	int err;

	buf.type = GIP_BUF_AUDIO;

	/* returns ENOSPC if no buffer is available */
	err = adap->ops->get_buffer(adap, &buf);
	if (err) {
		dev_err(&client->dev, "%s: get buffer failed: %d\n",
			__func__, err);
		return err;
	}

	gip_copy_audio_samples(client, samples, buf.data);

	/* set actual length */
	buf.length = client->audio_config_out.packet_size *
		     adap->audio_packet_count;

	/* always fails on adapter removal */
	err = adap->ops->submit_buffer(adap, &buf);
	if (err)
		dev_dbg(&client->dev, "%s: submit buffer failed: %d\n",
			__func__, err);

	return err;
}
EXPORT_SYMBOL_GPL(gip_send_audio_samples);

int gip_enable_audio(struct gip_client *client)
{
	struct gip_adapter *adap = client->adapter;
	int err;

	if (!adap->ops->enable_audio)
		return 0;

	err = adap->ops->enable_audio(adap);
	if (err)
		dev_err(&client->dev, "%s: enable failed: %d\n",
			__func__, err);

	return err;
}
EXPORT_SYMBOL_GPL(gip_enable_audio);

int gip_init_audio_in(struct gip_client *client)
{
	struct gip_adapter *adap = client->adapter;
	int err;

	if (!adap->ops->init_audio_in)
		return 0;

	err = adap->ops->init_audio_in(adap);
	if (err)
		dev_err(&client->dev, "%s: init failed: %d\n", __func__, err);

	return err;
}
EXPORT_SYMBOL_GPL(gip_init_audio_in);

int gip_init_audio_out(struct gip_client *client)
{
	struct gip_adapter *adap = client->adapter;
	int err;

	if (!adap->ops->init_audio_out)
		return 0;

	err = adap->ops->init_audio_out(adap,
					client->audio_config_out.packet_size);
	if (err)
		dev_err(&client->dev, "%s: init failed: %d\n", __func__, err);

	return err;
}
EXPORT_SYMBOL_GPL(gip_init_audio_out);

void gip_disable_audio(struct gip_client *client)
{
	struct gip_adapter *adap = client->adapter;
	int err;

	if (!adap->ops->disable_audio)
		return;

	/* always fails on adapter removal */
	err = adap->ops->disable_audio(adap);
	if (err)
		dev_dbg(&client->dev, "%s: disable failed: %d\n",
			__func__, err);
}
EXPORT_SYMBOL_GPL(gip_disable_audio);

static int gip_make_audio_config(struct gip_client *client,
				 struct gip_audio_config *cfg)
{
	switch (cfg->format) {
	case GIP_AUD_FORMAT_24KHZ_MONO:
		cfg->channels = 1;
		cfg->sample_rate = 24000;
		break;
	case GIP_AUD_FORMAT_48KHZ_STEREO:
		cfg->channels = 2;
		cfg->sample_rate = 48000;
		break;
	default:
		dev_err(&client->dev, "%s: unknown format: 0x%02x\n",
			__func__, cfg->format);
		return -ENOTSUPP;
	}

	cfg->buffer_size = cfg->sample_rate * cfg->channels *
			   sizeof(s16) * GIP_AUDIO_INTERVAL / MSEC_PER_SEC;
	cfg->fragment_size = cfg->buffer_size /
			     client->adapter->audio_packet_count;
	cfg->packet_size = cfg->fragment_size + sizeof(struct gip_header);

	if (cfg->fragment_size > GIP_HDR_LENGTH)
		cfg->packet_size += sizeof(struct gip_pkt_audio_header);

	cfg->valid = true;

	dev_dbg(&client->dev, "%s: rate=%d/%d, buffer=%d\n", __func__,
		cfg->sample_rate, cfg->channels, cfg->buffer_size);

	return 0;
}

static struct gip_info_element *gip_parse_info_element(u8 *data, int len,
						       __le16 offset,
						       int item_length)
{
	struct gip_info_element *elem;
	u16 off = le16_to_cpu(offset);
	u8 count;
	int total;

	if (!off)
		return ERR_PTR(-ENOTSUPP);

	if (len < off + sizeof(count))
		return ERR_PTR(-EINVAL);

	count = data[off++];
	if (!count)
		return ERR_PTR(-ENOTSUPP);

	total = count * item_length;
	if (len < off + total)
		return ERR_PTR(-EINVAL);

	elem = kzalloc(sizeof(*elem) + total, GFP_ATOMIC);
	if (!elem)
		return ERR_PTR(-ENOMEM);

	elem->count = count;
	memcpy(elem->data, data + off, total);

	return elem;
}

static int gip_parse_external_commands(struct gip_client *client,
				       struct gip_pkt_identify *pkt,
				       u8 *data, int len)
{
	struct gip_info_element *cmds;
	struct gip_command_descriptor *desc;
	int i;

	cmds = gip_parse_info_element(data, len, pkt->external_commands_offset,
				      sizeof(*desc));
	if (IS_ERR(cmds)) {
		if (PTR_ERR(cmds) == -ENOTSUPP)
			return 0;

		dev_err(&client->dev, "%s: parse failed: %ld\n",
			__func__, PTR_ERR(cmds));
		return PTR_ERR(cmds);
	}

	for (i = 0; i < cmds->count; i++) {
		desc = (struct gip_command_descriptor *)cmds->data + i;
		dev_dbg(&client->dev,
			"%s: command=0x%02x, length=0x%02x, options=0x%02x\n",
			__func__, desc->command, desc->length, desc->options);
	}

	client->external_commands = cmds;

	return 0;
}

static int gip_parse_audio_formats(struct gip_client *client,
				   struct gip_pkt_identify *pkt,
				   u8 *data, int len)
{
	struct gip_info_element *fmts;

	fmts = gip_parse_info_element(data, len,
				      pkt->audio_formats_offset, 2);
	if (IS_ERR(fmts)) {
		if (PTR_ERR(fmts) == -ENOTSUPP)
			return 0;

		dev_err(&client->dev, "%s: parse failed: %ld\n",
			__func__, PTR_ERR(fmts));
		return PTR_ERR(fmts);
	}

	dev_dbg(&client->dev, "%s: formats=%*phD\n", __func__,
		fmts->count * 2, fmts->data);
	client->audio_formats = fmts;

	return 0;
}

static int gip_parse_capabilities(struct gip_client *client,
				  struct gip_pkt_identify *pkt,
				  u8 *data, int len)
{
	struct gip_info_element *caps;

	caps = gip_parse_info_element(data, len,
				      pkt->capabilities_out_offset, 1);
	if (IS_ERR(caps)) {
		dev_err(&client->dev, "%s: parse out failed: %ld\n",
			__func__, PTR_ERR(caps));
		return PTR_ERR(caps);
	}

	dev_dbg(&client->dev, "%s: out=%*phD\n", __func__,
		caps->count, caps->data);
	client->capabilities_out = caps;

	caps = gip_parse_info_element(data, len,
				      pkt->capabilities_in_offset, 1);
	if (IS_ERR(caps)) {
		dev_err(&client->dev, "%s: parse in failed: %ld\n",
			__func__, PTR_ERR(caps));
		return PTR_ERR(caps);
	}

	dev_dbg(&client->dev, "%s: in=%*phD\n", __func__,
		caps->count, caps->data);
	client->capabilities_in = caps;

	return 0;
}

static int gip_parse_classes(struct gip_client *client,
			     struct gip_pkt_identify *pkt,
			     u8 *data, int len)
{
	struct gip_classes *classes;
	u16 off = le16_to_cpu(pkt->classes_offset);
	u8 count;
	u16 str_len;
	char *str;

	if (len < off + sizeof(count))
		return -EINVAL;

	/* number of individual strings */
	count = data[off++];
	if (!count)
		return -EINVAL;

	classes = kzalloc(sizeof(*classes) + sizeof(char *) * count,
			  GFP_ATOMIC);
	if (!classes)
		return -ENOMEM;

	client->classes = classes;

	while (classes->count < count) {
		if (len < off + sizeof(str_len))
			return -EINVAL;

		str_len = le16_to_cpup((u16 *)(data + off));
		off += sizeof(str_len);
		if (!str_len || len < off + str_len)
			return -EINVAL;

		/* null-terminated string */
		str = kzalloc(str_len + 1, GFP_ATOMIC);
		if (!str)
			return -ENOMEM;

		memcpy(str, data + off, str_len);
		classes->strings[classes->count] = str;
		classes->count++;
		off += str_len;

		dev_dbg(&client->dev, "%s: class=%s\n", __func__, str);
	}

	return 0;
}

static int gip_parse_interfaces(struct gip_client *client,
				struct gip_pkt_identify *pkt,
				u8 *data, int len)
{
	struct gip_info_element *intfs;
	guid_t *guid;
	int i;

	intfs = gip_parse_info_element(data, len, pkt->interfaces_offset,
				       sizeof(guid_t));
	if (IS_ERR(intfs)) {
		dev_err(&client->dev, "%s: parse failed: %ld\n",
			__func__, PTR_ERR(intfs));
		return PTR_ERR(intfs);
	}

	for (i = 0; i < intfs->count; i++) {
		guid = (guid_t *)intfs->data + i;
		dev_dbg(&client->dev, "%s: guid=%pUb\n", __func__, guid);
	}

	client->interfaces = intfs;

	return 0;
}

static int gip_parse_hid_descriptor(struct gip_client *client,
				    struct gip_pkt_identify *pkt,
				    u8 *data, int len)
{
	struct gip_info_element *desc;

	desc = gip_parse_info_element(data, len,
				      pkt->hid_descriptor_offset, 1);
	if (IS_ERR(desc)) {
		if (PTR_ERR(desc) == -ENOTSUPP)
			return 0;

		dev_err(&client->dev, "%s: parse failed: %ld\n",
			__func__, PTR_ERR(desc));
		return PTR_ERR(desc);
	}

	dev_dbg(&client->dev, "%s: length=0x%02x\n", __func__, desc->count);
	client->hid_descriptor = desc;

	return 0;
}

static int gip_handle_pkt_announce(struct gip_client *client,
				   struct gip_header *hdr,
				   void *data, int len)
{
	struct gip_pkt_announce *pkt = data;
	struct gip_hardware *hw = &client->hardware;

	if (len != hdr->length || len != sizeof(*pkt))
		return -EINVAL;

	if (atomic_read(&client->state) != GIP_CL_CONNECTED) {
		dev_warn(&client->dev, "%s: invalid state\n", __func__);
		return 0;
	}

	hw->vendor = le16_to_cpu(pkt->vendor_id);
	hw->product = le16_to_cpu(pkt->product_id);
	hw->version = (le16_to_cpu(pkt->fw_version.major) << 8) |
		      le16_to_cpu(pkt->fw_version.minor);

	dev_dbg(&client->dev,
		"%s: address=%pM, vendor=0x%04x, product=0x%04x\n",
		__func__, pkt->address, hw->vendor, hw->product);
	dev_dbg(&client->dev,
		"%s: firmware=%u.%u.%u.%u, hardware=%u.%u.%u.%u\n",
		__func__,
		le16_to_cpu(pkt->fw_version.major),
		le16_to_cpu(pkt->fw_version.minor),
		le16_to_cpu(pkt->fw_version.build),
		le16_to_cpu(pkt->fw_version.revision),
		le16_to_cpu(pkt->hw_version.major),
		le16_to_cpu(pkt->hw_version.minor),
		le16_to_cpu(pkt->hw_version.build),
		le16_to_cpu(pkt->hw_version.revision));

	atomic_set(&client->state, GIP_CL_ANNOUNCED);

	return gip_request_identification(client);
}

static int gip_handle_pkt_status(struct gip_client *client,
				 struct gip_header *hdr,
				 void *data, int len)
{
	struct gip_pkt_status *pkt = data;

	/* some devices occasionally send larger status packets */
	if (len != hdr->length || len < sizeof(*pkt))
		return -EINVAL;

	if (!(pkt->status & GIP_STATUS_CONNECTED)) {
		/* schedule client removal */
		dev_dbg(&client->dev, "%s: disconnected\n", __func__);
		gip_unregister_client(client);
		return 0;
	}

	if (!client->drv || !client->drv->ops.battery)
		return 0;

	return client->drv->ops.battery(client,
					FIELD_GET(GIP_BATT_TYPE, pkt->status),
					FIELD_GET(GIP_BATT_LEVEL, pkt->status));
}

static int gip_handle_pkt_identify(struct gip_client *client,
				   struct gip_header *hdr,
				   void *data, int len)
{
	struct gip_pkt_identify *pkt = data;
	int err;

	if (len < sizeof(*pkt))
		return -EINVAL;

	if (atomic_read(&client->state) != GIP_CL_ANNOUNCED) {
		dev_warn(&client->dev, "%s: invalid state\n", __func__);
		return 0;
	}

	/* skip unknown header */
	data += sizeof(pkt->unknown);
	len -= sizeof(pkt->unknown);

	err = gip_parse_external_commands(client, pkt, data, len);
	if (err)
		goto err_free_info;

	err = gip_parse_audio_formats(client, pkt, data, len);
	if (err)
		goto err_free_info;

	err = gip_parse_capabilities(client, pkt, data, len);
	if (err)
		goto err_free_info;

	err = gip_parse_classes(client, pkt, data, len);
	if (err)
		goto err_free_info;

	err = gip_parse_interfaces(client, pkt, data, len);
	if (err)
		goto err_free_info;

	err = gip_parse_hid_descriptor(client, pkt, data, len);
	if (err)
		goto err_free_info;

	/* schedule client registration */
	gip_register_client(client);

	return 0;

err_free_info:
	gip_free_client_info(client);

	return err;
}

static int gip_handle_pkt_guide_button(struct gip_client *client,
				       struct gip_header *hdr,
				       void *data, int len)
{
	struct gip_pkt_guide_button *pkt = data;

	if (len != hdr->length || len != sizeof(*pkt))
		return -EINVAL;

	if (!client->drv || !client->drv->ops.guide_button)
		return 0;

	return client->drv->ops.guide_button(client, pkt->pressed);
}

static int gip_handle_pkt_audio_format_chat(struct gip_client *client,
					    struct gip_header *hdr,
					    void *data, int len)
{
	struct gip_pkt_audio_format_chat *pkt = data;
	struct gip_audio_config *in = &client->audio_config_in;
	struct gip_audio_config *out = &client->audio_config_out;
	int err;

	if (len != hdr->length || len != sizeof(*pkt))
		return -EINVAL;

	/* chat headsets apparently default to 24 kHz */
	if (pkt->in_out != GIP_AUD_FORMAT_CHAT_24KHZ || in->valid || out->valid)
		return -EPROTO;

	err = gip_make_audio_config(client, in);
	if (err)
		return err;

	err = gip_make_audio_config(client, out);
	if (err)
		return err;

	if (!client->drv || !client->drv->ops.audio_ready)
		return 0;

	return client->drv->ops.audio_ready(client);
}

static int gip_handle_pkt_audio_volume_chat(struct gip_client *client,
					    struct gip_header *hdr,
					    void *data, int len)
{
	struct gip_pkt_audio_volume_chat *pkt = data;

	if (len != hdr->length || len != sizeof(*pkt))
		return -EINVAL;

	if (!client->drv || !client->drv->ops.audio_volume)
		return 0;

	return client->drv->ops.audio_volume(client, pkt->in, pkt->out);
}

static int gip_handle_pkt_audio_format(struct gip_client *client,
				       struct gip_header *hdr,
				       void *data, int len)
{
	struct gip_pkt_audio_format *pkt = data;
	struct gip_audio_config *in = &client->audio_config_in;
	struct gip_audio_config *out = &client->audio_config_out;
	int err;

	if (len != hdr->length || len != sizeof(*pkt))
		return -EINVAL;

	/* format has already been accepted */
	if (in->valid || out->valid)
		return -EPROTO;

	/* client rejected format, accept new format */
	if (pkt->in != in->format || pkt->out != out->format) {
		dev_warn(&client->dev, "%s: rejected: 0x%02x/0x%02x\n",
			 __func__, in->format, out->format);
		return gip_suggest_audio_format(client, pkt->in, pkt->out);
	}

	err = gip_make_audio_config(client, in);
	if (err)
		return err;

	err = gip_make_audio_config(client, out);
	if (err)
		return err;

	if (!client->drv || !client->drv->ops.audio_ready)
		return 0;

	return client->drv->ops.audio_ready(client);
}

static int gip_handle_pkt_audio_volume(struct gip_client *client,
				       struct gip_header *hdr,
				       void *data, int len)
{
	struct gip_pkt_audio_volume *pkt = data;

	if (len != hdr->length || len != sizeof(*pkt))
		return -EINVAL;

	if (!client->drv || !client->drv->ops.audio_volume)
		return 0;

	return client->drv->ops.audio_volume(client, pkt->in, pkt->out);
}

static int gip_handle_pkt_audio_control(struct gip_client *client,
					struct gip_header *hdr,
					void *data, int len)
{
	struct gip_pkt_audio_control *pkt = data;

	if (len < sizeof(*pkt))
		return -EINVAL;

	switch (pkt->subcommand) {
	case GIP_AUD_CTRL_FORMAT_CHAT:
		return gip_handle_pkt_audio_format_chat(client, hdr, data, len);
	case GIP_AUD_CTRL_VOLUME_CHAT:
		return gip_handle_pkt_audio_volume_chat(client, hdr, data, len);
	case GIP_AUD_CTRL_FORMAT:
		return gip_handle_pkt_audio_format(client, hdr, data, len);
	case GIP_AUD_CTRL_VOLUME:
		return gip_handle_pkt_audio_volume(client, hdr, data, len);
	}

	dev_err(&client->dev, "%s: unknown subcommand: 0x%02x\n",
		__func__, pkt->subcommand);

	return -EPROTO;
}

static int gip_handle_pkt_hid_report(struct gip_client *client,
				     struct gip_header *hdr,
				     void *data, int len)
{
	if (len != hdr->length)
		return -EINVAL;

	if (!client->drv || !client->drv->ops.hid_report)
		return 0;

	return client->drv->ops.hid_report(client, data, len);
}

static int gip_handle_pkt_audio_samples(struct gip_client *client,
					struct gip_header *hdr,
					void *data, int len)
{
	struct gip_pkt_audio_header *pkt = data;
	u16 total = hdr->length & GIP_HDR_LENGTH;

	if (len < sizeof(*pkt))
		return -EINVAL;

	/* wireless clients use extended audio headers */
	if (hdr->length & GIP_HDR_EXTENDED) {
		total |= (pkt->length_extra & GENMASK(3, 0)) << 7;
		if (total < sizeof(*pkt) + 2 || len < total)
			return -EINVAL;

		data += sizeof(*pkt) + 2;
		total -= sizeof(*pkt) + 2;
	} else {
		if (total < sizeof(*pkt) || len < total)
			return -EINVAL;

		data += sizeof(*pkt);
		total -= sizeof(*pkt);
	}

	if (!client->drv || !client->drv->ops.audio_samples)
		return 0;

	return client->drv->ops.audio_samples(client, data, total);
}

static int gip_dispatch_pkt_internal(struct gip_client *client,
				     struct gip_header *hdr,
				     void *data, int len)
{
	switch (hdr->command) {
	case GIP_CMD_ANNOUNCE:
		return gip_handle_pkt_announce(client, hdr, data, len);
	case GIP_CMD_STATUS:
		return gip_handle_pkt_status(client, hdr, data, len);
	case GIP_CMD_IDENTIFY:
		return gip_handle_pkt_identify(client, hdr, data, len);
	case GIP_CMD_GUIDE_BUTTON:
		return gip_handle_pkt_guide_button(client, hdr, data, len);
	case GIP_CMD_AUDIO_CONTROL:
		return gip_handle_pkt_audio_control(client, hdr, data, len);
	case GIP_CMD_HID_REPORT:
		return gip_handle_pkt_hid_report(client, hdr, data, len);
	case GIP_CMD_AUDIO_SAMPLES:
		return gip_handle_pkt_audio_samples(client, hdr, data, len);
	}

	return 0;
}

static int gip_handle_pkt_input(struct gip_client *client,
				struct gip_header *hdr,
				void *data, int len)
{
	if (len != hdr->length)
		return -EINVAL;

	if (!client->drv || !client->drv->ops.input)
		return 0;

	return client->drv->ops.input(client, data, len);
}

static int gip_dispatch_pkt(struct gip_client *client,
			    struct gip_header *hdr,
			    void *data, int len)
{
	if (hdr->options & GIP_OPT_INTERNAL)
		return gip_dispatch_pkt_internal(client, hdr, data, len);

	switch (hdr->command) {
	case GIP_CMD_INPUT:
		return gip_handle_pkt_input(client, hdr, data, len);
	}

	return 0;
}

static int gip_parse_chunk(struct gip_client *client,
			   struct gip_chunk *chunk,
			   void *data, int len)
{
	struct gip_header *hdr = data;
	struct gip_chunk_header *chunk_hdr = data + sizeof(*hdr);

	if (len < sizeof(*hdr) + sizeof(*chunk_hdr)) {
		dev_err(&client->dev, "%s: invalid length\n", __func__);
		return -EINVAL;
	}

	if (hdr->length & GIP_HDR_EXTENDED)
		chunk->offset = chunk_hdr->offset;
	else
		chunk->offset = (chunk_hdr->offset << 7) |
				(chunk_hdr->offset_extra & GENMASK(6, 0));

	chunk->data = data + sizeof(*hdr) + sizeof(*chunk_hdr);
	chunk->length = len - sizeof(*hdr) - sizeof(*chunk_hdr);

	if (chunk->length != (hdr->length & GIP_HDR_LENGTH)) {
		dev_err(&client->dev, "%s: length mismatch\n", __func__);
		return -EINVAL;
	}

	dev_dbg(&client->dev, "%s: offset=0x%02x, length=0x%02x\n",
		__func__, chunk->offset, chunk->length);

	return 0;
}

static int gip_init_chunk_buffer(struct gip_client *client, int len)
{
	struct gip_chunk_buffer *buf = client->chunk_buf;

	if (buf) {
		dev_err(&client->dev, "%s: already initialized\n", __func__);
		kfree(buf);
	}

	buf = kzalloc(sizeof(*buf) + len, GFP_ATOMIC);
	if (!buf)
		return -ENOMEM;

	dev_dbg(&client->dev, "%s: length=0x%02x\n", __func__, len);
	buf->length = len;
	client->chunk_buf = buf;

	return 0;
}

static int gip_copy_chunk_data(struct gip_client *client,
			       struct gip_chunk chunk)
{
	struct gip_chunk_buffer *buf = client->chunk_buf;

	if (!buf) {
		dev_err(&client->dev, "%s: buffer not allocated\n", __func__);
		return -EPROTO;
	}

	if (buf->full) {
		dev_err(&client->dev, "%s: buffer full\n", __func__);
		return -ENOMEM;
	}

	if (chunk.offset + chunk.length > buf->length) {
		dev_err(&client->dev, "%s: buffer too small\n", __func__);
		return -EINVAL;
	}

	if (chunk.length) {
		memcpy(buf->data + chunk.offset, chunk.data, chunk.length);
	} else {
		/* empty chunk signals the completion of the transfer */
		/* offset *should* be the total length of all chunks */
		/* certain third party devices report an incorrect length */
		if (chunk.offset != buf->length)
			dev_warn(&client->dev, "%s: length mismatch\n",
				 __func__);

		dev_dbg(&client->dev, "%s: buffer complete\n", __func__);
		buf->full = true;
	}

	return 0;
}

static int gip_process_chunk(struct gip_client *client, void *data, int len)
{
	struct gip_header *hdr = data;
	struct gip_chunk chunk = {};
	int err;

	err = gip_parse_chunk(client, &chunk, data, len);
	if (err)
		return err;

	if (hdr->options & GIP_OPT_CHUNK_START) {
		/* offset is total length of all chunks */
		err = gip_init_chunk_buffer(client, chunk.offset);
		if (err)
			return err;

		/* acknowledge with remaining length */
		err = gip_acknowledge_pkt(client, hdr, chunk.length,
					  client->chunk_buf->length -
					  chunk.length);
		if (err)
			return err;

		chunk.offset = 0;
	} else if (hdr->options & GIP_OPT_ACKNOWLEDGE) {
		/* acknowledge with total buffer length */
		err = gip_acknowledge_pkt(client, hdr,
					  client->chunk_buf->length, 0);
		if (err)
			return err;
	}

	return gip_copy_chunk_data(client, chunk);
}

static int gip_process_pkt_coherent(struct gip_client *client,
				    void *data, int len)
{
	struct gip_header *hdr = data;
	int err;

	if (hdr->options & GIP_OPT_ACKNOWLEDGE) {
		err = gip_acknowledge_pkt(client, hdr, hdr->length, 0);
		if (err)
			return err;
	}

	return gip_dispatch_pkt(client, hdr,
				data + sizeof(*hdr), len - sizeof(*hdr));
}

static int gip_process_pkt_chunked(struct gip_client *client,
				   void *data, int len)
{
	struct gip_header *hdr = data;
	struct gip_chunk_buffer *buf;
	int err;

	err = gip_process_chunk(client, data, len);
	if (err)
		return err;

	/* all chunks have been received */
	buf = client->chunk_buf;
	if (buf->full) {
		err = gip_dispatch_pkt(client, hdr, buf->data, buf->length);

		kfree(buf);
		client->chunk_buf = NULL;
	}

	return err;
}

int gip_process_buffer(struct gip_adapter *adap, void *data, int len)
{
	struct gip_header *hdr = data;
	struct gip_client *client;
	u8 id = hdr->options & GIP_HDR_CLIENT_ID;
	int err = 0;
	unsigned long flags;

	if (len < sizeof(*hdr)) {
		dev_err(&adap->dev, "%s: invalid length\n", __func__);
		return -EINVAL;
	}

	client = gip_get_or_init_client(adap, id);
	if (IS_ERR(client)) {
		dev_err(&adap->dev, "%s: get/init client failed: %ld\n",
			__func__, PTR_ERR(client));
		return PTR_ERR(client);
	}

	spin_lock_irqsave(&client->lock, flags);

	if (atomic_read(&client->state) == GIP_CL_DISCONNECTED)
		goto err_unlock;

	if (hdr->options & GIP_OPT_CHUNK)
		err = gip_process_pkt_chunked(client, data, len);
	else
		err = gip_process_pkt_coherent(client, data, len);

	if (err) {
		dev_err(&adap->dev, "%s: process packet failed: %d\n",
			__func__, err);
		print_hex_dump_debug("packet: ", DUMP_PREFIX_NONE, 16, 1,
				     data, len, false);
	}

err_unlock:
	spin_unlock_irqrestore(&client->lock, flags);
	gip_put_client(client);

	return err;
}
EXPORT_SYMBOL_GPL(gip_process_buffer);
