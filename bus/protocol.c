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
#define GIP_HDR_MIN_LENGTH 3

#define GIP_CHUNK_BUF_MAX_LENGTH 0xffff

#define GIP_BATT_LEVEL GENMASK(1, 0)
#define GIP_BATT_TYPE GENMASK(3, 2)
#define GIP_STATUS_CONNECTED BIT(7)

#define GIP_VKEY_LEFT_WIN 0x5b

enum gip_command_internal {
	GIP_CMD_ACKNOWLEDGE = 0x01,
	GIP_CMD_ANNOUNCE = 0x02,
	GIP_CMD_STATUS = 0x03,
	GIP_CMD_IDENTIFY = 0x04,
	GIP_CMD_POWER = 0x05,
	GIP_CMD_AUTHENTICATE = 0x06,
	GIP_CMD_VIRTUAL_KEY = 0x07,
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
	u32 packet_length;
	u32 chunk_offset;
};

struct gip_pkt_acknowledge {
	u8 unknown;
	u8 command;
	u8 options;
	__le16 length;
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
	__le16 firmware_versions_offset;
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

struct gip_pkt_virtual_key {
	u8 down;
	u8 key;
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

struct gip_pkt_audio_samples {
	__le16 length_out;
	u8 samples[];
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

struct gip_firmware_version {
	__le16 major;
	__le16 minor;
} __packed;

static int gip_encode_varint(u8 *buf, u32 val)
{
	int i;

	/* encode variable-length integer */
	for (i = 0; i < sizeof(val); i++) {
		buf[i] = val;
		if (val > GENMASK(6, 0))
			buf[i] |= BIT(7);

		val >>= 7;
		if (!val)
			break;
	}

	return i + 1;
}

static int gip_decode_varint(u8 *data, int len, u32 *val)
{
	int i;

	/* decode variable-length integer */
	for (i = 0; i < sizeof(*val) && i < len; i++) {
		*val |= (data[i] & GENMASK(6, 0)) << (i * 7);

		if (!(data[i] & BIT(7)))
			break;
	}

	return i + 1;
}

static int gip_get_actual_header_length(struct gip_header *hdr)
{
	u32 pkt_len = hdr->packet_length;
	u32 chunk_offset = hdr->chunk_offset;
	int len = GIP_HDR_MIN_LENGTH;

	do {
		len++;
		pkt_len >>= 7;
	} while (pkt_len);

	if (hdr->options & GIP_OPT_CHUNK) {
		while (chunk_offset) {
			len++;
			chunk_offset >>= 7;
		}
	}

	return len;
}

static int gip_get_header_length(struct gip_header *hdr)
{
	int len = gip_get_actual_header_length(hdr);

	/* round up to nearest even length */
	return len + (len % 2);
}

static void gip_encode_header(struct gip_header *hdr, u8 *buf)
{
	int hdr_len = 0;

	buf[hdr_len++] = hdr->command;
	buf[hdr_len++] = hdr->options;
	buf[hdr_len++] = hdr->sequence;

	hdr_len += gip_encode_varint(buf + hdr_len, hdr->packet_length);

	/* header length must be even */
	if (gip_get_actual_header_length(hdr) % 2) {
		buf[hdr_len - 1] |= BIT(7);
		buf[hdr_len++] = 0;
	}

	if (hdr->options & GIP_OPT_CHUNK)
		gip_encode_varint(buf + hdr_len, hdr->chunk_offset);
}

static int gip_decode_header(struct gip_header *hdr, u8 *data, int len)
{
	int hdr_len = 0;

	hdr->command = data[hdr_len++];
	hdr->options = data[hdr_len++];
	hdr->sequence = data[hdr_len++];
	hdr->packet_length = 0;
	hdr->chunk_offset = 0;

	hdr_len += gip_decode_varint(data + hdr_len, len - hdr_len,
				     &hdr->packet_length);

	if (hdr->options & GIP_OPT_CHUNK)
		hdr_len += gip_decode_varint(data + hdr_len, len - hdr_len,
					     &hdr->chunk_offset);

	return hdr_len;
}

static int gip_send_pkt(struct gip_client *client,
			struct gip_header *hdr, void *data)
{
	struct gip_adapter *adap = client->adapter;
	struct gip_adapter_buffer buf = {};
	int hdr_len, err;
	unsigned long flags;

	buf.type = GIP_BUF_DATA;

	spin_lock_irqsave(&adap->send_lock, flags);

	err = adap->ops->get_buffer(adap, &buf);
	if (err) {
		dev_err(&client->dev, "%s: get buffer failed: %d\n",
			__func__, err);
		goto err_unlock;
	}

	hdr_len = gip_get_header_length(hdr);
	if (buf.length < hdr_len + hdr->packet_length) {
		err = -ENOSPC;
		goto err_unlock;
	}

	/* sequence number is always greater than zero */
	while (!hdr->sequence)
		hdr->sequence = adap->data_sequence++;

	gip_encode_header(hdr, buf.data);
	if (data)
		memcpy(buf.data + hdr_len, data, hdr->packet_length);

	/* set actual length */
	buf.length = hdr_len + hdr->packet_length;

	/* always fails on adapter removal */
	err = adap->ops->submit_buffer(adap, &buf);
	if (err)
		dev_dbg(&client->dev, "%s: submit buffer failed: %d\n",
			__func__, err);

err_unlock:
	spin_unlock_irqrestore(&adap->send_lock, flags);

	return err;
}

static int gip_acknowledge_pkt(struct gip_client *client,
			       struct gip_header *ack)
{
	struct gip_chunk_buffer *chunk_buf = client->chunk_buf;
	struct gip_header hdr = {};
	struct gip_pkt_acknowledge pkt = {};

	hdr.command = GIP_CMD_ACKNOWLEDGE;
	hdr.options = client->id | GIP_OPT_INTERNAL;
	hdr.sequence = ack->sequence;
	hdr.packet_length = sizeof(pkt);

	pkt.command = ack->command;
	pkt.options = client->id | GIP_OPT_INTERNAL;
	pkt.length = cpu_to_le16(ack->chunk_offset + ack->packet_length);

	if ((ack->options & GIP_OPT_CHUNK) && chunk_buf)
		pkt.remaining = cpu_to_le16(chunk_buf->length) - pkt.length;

	return gip_send_pkt(client, &hdr, &pkt);
}

static int gip_request_identification(struct gip_client *client)
{
	struct gip_header hdr = {};

	hdr.command = GIP_CMD_IDENTIFY;
	hdr.options = client->id | GIP_OPT_INTERNAL;

	return gip_send_pkt(client, &hdr, NULL);
}

int gip_set_power_mode(struct gip_client *client, enum gip_power_mode mode)
{
	struct gip_header hdr = {};
	struct gip_pkt_power pkt = {};

	hdr.command = GIP_CMD_POWER;
	hdr.options = client->id | GIP_OPT_INTERNAL;
	hdr.packet_length = sizeof(pkt);

	pkt.mode = mode;

	return gip_send_pkt(client, &hdr, &pkt);
}
EXPORT_SYMBOL_GPL(gip_set_power_mode);

int gip_complete_authentication(struct gip_client *client)
{
	struct gip_header hdr = {};
	struct gip_pkt_authenticate pkt = {};

	hdr.command = GIP_CMD_AUTHENTICATE;
	hdr.options = client->id | GIP_OPT_INTERNAL;
	hdr.packet_length = sizeof(pkt);

	pkt.unknown1 = 0x01;

	return gip_send_pkt(client, &hdr, &pkt);
}
EXPORT_SYMBOL_GPL(gip_complete_authentication);

static int gip_set_audio_format_chat(struct gip_client *client,
				     enum gip_audio_format_chat in_out)
{
	struct gip_header hdr = {};
	struct gip_pkt_audio_format_chat pkt = {};

	hdr.command = GIP_CMD_AUDIO_CONTROL;
	hdr.options = client->id | GIP_OPT_INTERNAL;
	hdr.packet_length = sizeof(pkt);

	pkt.control.subcommand = GIP_AUD_CTRL_FORMAT_CHAT;
	pkt.in_out = in_out;

	return gip_send_pkt(client, &hdr, &pkt);
}

static int gip_set_audio_format(struct gip_client *client,
				enum gip_audio_format in,
				enum gip_audio_format out)
{
	struct gip_header hdr = {};
	struct gip_pkt_audio_format pkt = {};

	hdr.command = GIP_CMD_AUDIO_CONTROL;
	hdr.options = client->id | GIP_OPT_INTERNAL;
	hdr.packet_length = sizeof(pkt);

	pkt.control.subcommand = GIP_AUD_CTRL_FORMAT;
	pkt.in = in;
	pkt.out = out;

	return gip_send_pkt(client, &hdr, &pkt);
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
	hdr.packet_length = sizeof(pkt);

	pkt.control.subcommand = GIP_AUD_CTRL_VOLUME;
	pkt.mute = GIP_AUD_VOLUME_UNMUTED;
	pkt.out = out;
	pkt.in = in;

	return gip_send_pkt(client, &hdr, &pkt);
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

int gip_send_rumble(struct gip_client *client, void *pkt, u32 len)
{
	struct gip_header hdr = {};

	hdr.command = GIP_CMD_RUMBLE;
	hdr.options = client->id;
	hdr.packet_length = len;

	return gip_send_pkt(client, &hdr, pkt);
}
EXPORT_SYMBOL_GPL(gip_send_rumble);

int gip_set_led_mode(struct gip_client *client,
		     enum gip_led_mode mode, u8 brightness)
{
	struct gip_header hdr = {};
	struct gip_pkt_led pkt = {};

	hdr.command = GIP_CMD_LED;
	hdr.options = client->id | GIP_OPT_INTERNAL;
	hdr.packet_length = sizeof(pkt);

	pkt.mode = mode;
	pkt.brightness = brightness;

	return gip_send_pkt(client, &hdr, &pkt);
}
EXPORT_SYMBOL_GPL(gip_set_led_mode);

static void gip_copy_audio_samples(struct gip_client *client,
				   void *samples, void *buf)
{
	struct gip_audio_config *cfg = &client->audio_config_out;
	struct gip_header hdr = {};
	void *src, *dest;
	int hdr_len, i;

	hdr.command = GIP_CMD_AUDIO_SAMPLES;
	hdr.options = client->id | GIP_OPT_INTERNAL;
	hdr.packet_length = cfg->fragment_size;

	hdr_len = gip_get_header_length(&hdr);

	for (i = 0; i < client->adapter->audio_packet_count; i++) {
		src = samples + i * cfg->fragment_size;
		dest = buf + i * cfg->packet_size;

		/* sequence number is always greater than zero */
		do {
			hdr.sequence = client->adapter->audio_sequence++;
		} while (!hdr.sequence);

		gip_encode_header(&hdr, dest);
		memcpy(dest + hdr_len, src, cfg->fragment_size);
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
	struct gip_header hdr = {};

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

	/* pseudo header for length calculation */
	hdr.packet_length = cfg->fragment_size;
	cfg->packet_size = gip_get_header_length(&hdr) + cfg->fragment_size;
	cfg->valid = true;

	dev_dbg(&client->dev, "%s: rate=%d/%d, buffer=%d\n", __func__,
		cfg->sample_rate, cfg->channels, cfg->buffer_size);

	return 0;
}

static struct gip_info_element *gip_parse_info_element(u8 *data, u32 len,
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
				       u8 *data, u32 len)
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

static int gip_parse_firmware_versions(struct gip_client *client,
				       struct gip_pkt_identify *pkt,
				       u8 *data, u32 len)
{
	struct gip_info_element *vers;
	struct gip_firmware_version *ver;
	int i;

	vers = gip_parse_info_element(data, len, pkt->firmware_versions_offset,
				      sizeof(*ver));
	if (IS_ERR(vers)) {
		dev_err(&client->dev, "%s: parse failed: %ld\n",
			__func__, PTR_ERR(vers));
		return PTR_ERR(vers);
	}

	for (i = 0; i < vers->count; i++) {
		ver = (struct gip_firmware_version *)vers->data + i;
		dev_dbg(&client->dev, "%s: version=%u.%u\n", __func__,
			le16_to_cpu(ver->major), le16_to_cpu(ver->minor));
	}

	client->firmware_versions = vers;

	return 0;
}

static int gip_parse_audio_formats(struct gip_client *client,
				   struct gip_pkt_identify *pkt,
				   u8 *data, u32 len)
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
				  u8 *data, u32 len)
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
			     u8 *data, u32 len)
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
				u8 *data, u32 len)
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
				    u8 *data, u32 len)
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
				   void *data, u32 len)
{
	struct gip_pkt_announce *pkt = data;
	struct gip_hardware *hw = &client->hardware;

	if (len != sizeof(*pkt))
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
				 void *data, u32 len)
{
	struct gip_pkt_status *pkt = data;

	/* some devices occasionally send larger status packets */
	if (len < sizeof(*pkt))
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
				   void *data, u32 len)
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

	err = gip_parse_firmware_versions(client, pkt, data, len);
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

static int gip_handle_pkt_virtual_key(struct gip_client *client,
				      void *data, u32 len)
{
	struct gip_pkt_virtual_key *pkt = data;

	if (len != sizeof(*pkt))
		return -EINVAL;

	if (pkt->key != GIP_VKEY_LEFT_WIN)
		return -EINVAL;

	if (!client->drv || !client->drv->ops.guide_button)
		return 0;

	return client->drv->ops.guide_button(client, pkt->down);
}

static int gip_handle_pkt_audio_format_chat(struct gip_client *client,
					    void *data, u32 len)
{
	struct gip_pkt_audio_format_chat *pkt = data;
	struct gip_audio_config *in = &client->audio_config_in;
	struct gip_audio_config *out = &client->audio_config_out;
	int err;

	if (len != sizeof(*pkt))
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
					    void *data, u32 len)
{
	struct gip_pkt_audio_volume_chat *pkt = data;

	if (len != sizeof(*pkt))
		return -EINVAL;

	if (!client->drv || !client->drv->ops.audio_volume)
		return 0;

	return client->drv->ops.audio_volume(client, pkt->in, pkt->out);
}

static int gip_handle_pkt_audio_format(struct gip_client *client,
				       void *data, u32 len)
{
	struct gip_pkt_audio_format *pkt = data;
	struct gip_audio_config *in = &client->audio_config_in;
	struct gip_audio_config *out = &client->audio_config_out;
	int err;

	if (len != sizeof(*pkt))
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
				       void *data, u32 len)
{
	struct gip_pkt_audio_volume *pkt = data;

	if (len != sizeof(*pkt))
		return -EINVAL;

	if (!client->drv || !client->drv->ops.audio_volume)
		return 0;

	return client->drv->ops.audio_volume(client, pkt->in, pkt->out);
}

static int gip_handle_pkt_audio_control(struct gip_client *client,
					void *data, u32 len)
{
	struct gip_pkt_audio_control *pkt = data;

	if (len < sizeof(*pkt))
		return -EINVAL;

	switch (pkt->subcommand) {
	case GIP_AUD_CTRL_FORMAT_CHAT:
		return gip_handle_pkt_audio_format_chat(client, data, len);
	case GIP_AUD_CTRL_VOLUME_CHAT:
		return gip_handle_pkt_audio_volume_chat(client, data, len);
	case GIP_AUD_CTRL_FORMAT:
		return gip_handle_pkt_audio_format(client, data, len);
	case GIP_AUD_CTRL_VOLUME:
		return gip_handle_pkt_audio_volume(client, data, len);
	}

	dev_err(&client->dev, "%s: unknown subcommand: 0x%02x\n",
		__func__, pkt->subcommand);

	return -EPROTO;
}

static int gip_handle_pkt_hid_report(struct gip_client *client,
				     void *data, u32 len)
{
	if (!client->drv || !client->drv->ops.hid_report)
		return 0;

	return client->drv->ops.hid_report(client, data, len);
}

static int gip_handle_pkt_input(struct gip_client *client,
				void *data, u32 len)
{
	if (!client->drv || !client->drv->ops.input)
		return 0;

	return client->drv->ops.input(client, data, len);
}

static int gip_handle_pkt_audio_samples(struct gip_client *client,
					void *data, u32 len)
{
	struct gip_pkt_audio_samples *pkt = data;

	if (len < sizeof(*pkt))
		return -EINVAL;

	if (!client->drv || !client->drv->ops.audio_samples)
		return 0;

	return client->drv->ops.audio_samples(client, pkt->samples,
					      len - sizeof(*pkt));
}

static int gip_dispatch_pkt(struct gip_client *client,
			    struct gip_header *hdr, void *data, u32 len)
{
	if (hdr->options & GIP_OPT_INTERNAL) {
		switch (hdr->command) {
		case GIP_CMD_ANNOUNCE:
			return gip_handle_pkt_announce(client, data, len);
		case GIP_CMD_STATUS:
			return gip_handle_pkt_status(client, data, len);
		case GIP_CMD_IDENTIFY:
			return gip_handle_pkt_identify(client, data, len);
		case GIP_CMD_VIRTUAL_KEY:
			return gip_handle_pkt_virtual_key(client, data, len);
		case GIP_CMD_AUDIO_CONTROL:
			return gip_handle_pkt_audio_control(client, data, len);
		case GIP_CMD_HID_REPORT:
			return gip_handle_pkt_hid_report(client, data, len);
		case GIP_CMD_AUDIO_SAMPLES:
			return gip_handle_pkt_audio_samples(client, data, len);
		default:
			return 0;
		}
	}

	switch (hdr->command) {
	case GIP_CMD_INPUT:
		return gip_handle_pkt_input(client, data, len);
	}

	return 0;
}

static int gip_init_chunk_buffer(struct gip_client *client, u32 len)
{
	struct gip_chunk_buffer *buf = client->chunk_buf;

	if (len > GIP_CHUNK_BUF_MAX_LENGTH)
		return -EINVAL;

	if (buf) {
		dev_err(&client->dev, "%s: already initialized\n", __func__);
		kfree(buf);
		client->chunk_buf = NULL;
	}

	buf = kzalloc(sizeof(*buf) + len, GFP_ATOMIC);
	if (!buf)
		return -ENOMEM;

	dev_dbg(&client->dev, "%s: length=0x%04x\n", __func__, len);
	buf->length = len;
	client->chunk_buf = buf;

	return 0;
}

static int gip_process_pkt_chunked(struct gip_client *client,
				   struct gip_header *hdr, void *data)
{
	struct gip_chunk_buffer *buf = client->chunk_buf;
	int err;

	dev_dbg(&client->dev, "%s: offset=0x%04x, length=0x%04x\n",
		__func__, hdr->chunk_offset, hdr->packet_length);

	if (!buf) {
		dev_err(&client->dev, "%s: buffer not allocated\n", __func__);
		return -EPROTO;
	}

	if (buf->length < hdr->chunk_offset + hdr->packet_length) {
		dev_err(&client->dev, "%s: buffer too small\n", __func__);
		return -EINVAL;
	}

	if (hdr->packet_length) {
		memcpy(buf->data + hdr->chunk_offset, data, hdr->packet_length);
		return 0;
	}

	/* empty chunk signals the completion of the transfer */
	/* offset (total transfer length) can be smaller than buffer */
	err = gip_dispatch_pkt(client, hdr, buf->data, hdr->chunk_offset);

	kfree(buf);
	client->chunk_buf = NULL;

	return err;
}

static int gip_process_pkt(struct gip_client *client,
			   struct gip_header *hdr, void *data)
{
	int err;

	if (hdr->options & GIP_OPT_CHUNK_START) {
		/* offset is total length of all chunks */
		err = gip_init_chunk_buffer(client, hdr->chunk_offset);
		if (err)
			return err;

		hdr->chunk_offset = 0;
	}

	if (hdr->options & GIP_OPT_ACKNOWLEDGE) {
		err = gip_acknowledge_pkt(client, hdr);
		if (err)
			return err;
	}

	if (hdr->options & GIP_OPT_CHUNK)
		return gip_process_pkt_chunked(client, hdr, data);

	return gip_dispatch_pkt(client, hdr, data, hdr->packet_length);
}

static int gip_process_adapter_pkt(struct gip_adapter *adap,
				   struct gip_header *hdr, void *data)
{
	struct gip_client *client;
	u8 id = hdr->options & GIP_HDR_CLIENT_ID;
	int err = 0;
	unsigned long flags;

	client = gip_get_or_init_client(adap, id);
	if (IS_ERR(client))
		return PTR_ERR(client);

	spin_lock_irqsave(&client->lock, flags);

	if (atomic_read(&client->state) != GIP_CL_DISCONNECTED)
		err = gip_process_pkt(client, hdr, data);

	spin_unlock_irqrestore(&client->lock, flags);
	gip_put_client(client);

	return err;
}

int gip_process_buffer(struct gip_adapter *adap, void *data, int len)
{
	struct gip_header hdr;
	int hdr_len, err;

	while (len > GIP_HDR_MIN_LENGTH) {
		hdr_len = gip_decode_header(&hdr, data, len);
		if (len < hdr_len + hdr.packet_length)
			return -EINVAL;

		err = gip_process_adapter_pkt(adap, &hdr, data + hdr_len);
		if (err)
			return err;

		data += hdr_len + hdr.packet_length;
		len -= hdr_len + hdr.packet_length;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(gip_process_buffer);
