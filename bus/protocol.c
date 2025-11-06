// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2021 Severin von Wnuck-Lipinski <severinvonw@outlook.de>
 */

#include <linux/slab.h>
#include <linux/bitfield.h>
#include <linux/uuid.h>

#include "bus.h"

#define GIP_HDR_CLIENT_ID GENMASK(3, 0)
#define GIP_HDR_MIN_LENGTH 3

/* max length, even for wireless packets (except audio) */
#define GIP_PKT_MAX_LENGTH 58

/* reliable packet transmission coalesce count */
#define GIP_PKT_COALESCE_COUNT 5

#define GIP_CHUNK_BUF_MAX_LENGTH 0xffff

#define GIP_BATT_LEVEL GENMASK(1, 0)
#define GIP_BATT_TYPE GENMASK(3, 2)
#define GIP_STATUS_CONNECTED BIT(7)

#define GIP_VKEY_LEFT_WIN 0x5b

#define gip_dbg(client, ...) dev_dbg(&(client)->adapter->dev, __VA_ARGS__)
#define gip_warn(client, ...) dev_warn(&(client)->adapter->dev, __VA_ARGS__)
#define gip_err(client, ...) dev_err(&(client)->adapter->dev, __VA_ARGS__)

enum gip_command_core {
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

enum gip_command_client {
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
	__le16 client_commands_offset;
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
	u8 chat;
	u8 in;
	u8 unknown1;
	u8 unknown2[2];
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

static int gip_init_chunk_buffer(struct gip_client *client,
				 struct gip_header *hdr,
				 struct gip_chunk_buffer **buf,
				 bool is_input)
{
	/* offset is total length of all chunks */
	if (hdr->chunk_offset > GIP_CHUNK_BUF_MAX_LENGTH)
		return -EINVAL;

	if (*buf) {
		gip_err(client, "%s: already initialized (%s)\n", __func__,
				is_input ? "in" : "out");
		kfree(*buf);
		*buf = NULL;
	}

	*buf = kzalloc(sizeof(**buf) + hdr->chunk_offset, GFP_ATOMIC);
	if (!*buf)
		return -ENOMEM;

	(*buf)->header = *hdr;
	/* clear GIP_OPT_ACKNOWLEDGE and GIP_OPT_CHUNK_START */
	(*buf)->header.options &= ~(GIP_OPT_ACKNOWLEDGE | GIP_OPT_CHUNK_START);
	(*buf)->length = hdr->chunk_offset;
	gip_dbg(client, "%s[%s]: command=0x%02x, length=0x%04x\n", __func__,
		is_input ? "in" : "out",
		(*buf)->header.command, (*buf)->length);

	return 0;
}

static int gip_send_pkt_simple(struct gip_client *client,
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
		gip_err(client, "%s: get buffer failed: %d\n", __func__, err);
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

	/* debug message sent */
	// gip_dbg(client, "%s: cmd=0x%02x len=0x%04x seq=0x%02x offset=0x%04x\n",
	// 	__func__, hdr->command, buf.length, hdr->sequence,
	// 	hdr->chunk_offset);

	/* always fails on adapter removal */
	err = adap->ops->submit_buffer(adap, &buf);
	if (err)
		gip_dbg(client, "%s: submit buffer failed: %d\n", __func__, err);

err_unlock:
	spin_unlock_irqrestore(&adap->send_lock, flags);

	return err;
}

static int gip_send_pkt(struct gip_client *client,
			struct gip_header *hdr, void *data)
{
	int err;

	/* packet fits into single buffer */
	if (hdr->packet_length <= GIP_PKT_MAX_LENGTH)
		return gip_send_pkt_simple(client, hdr, data);

	/* chunk offset of first chunk is total length */
	hdr->options |= GIP_OPT_ACKNOWLEDGE | GIP_OPT_CHUNK_START |
			GIP_OPT_CHUNK;
	hdr->chunk_offset = hdr->packet_length;
	hdr->packet_length = GIP_PKT_MAX_LENGTH;

	err = gip_send_pkt_simple(client, hdr, data);
	if (err)
		return err;

	/* allocate output buffer for all chunks */
	err = gip_init_chunk_buffer(client, hdr, &client->chunk_buf_in, false);
	if (err)
		return err;

	if (data)
		memcpy(client->chunk_buf_in->data, data, hdr->chunk_offset);

	return 0;
}

static int gip_acknowledge_pkt(struct gip_client *client,
			       struct gip_header *ack)
{
	struct gip_chunk_buffer *buf = client->chunk_buf_out;
	struct gip_header hdr = {};
	struct gip_pkt_acknowledge pkt = {};
	u32 len = ack->chunk_offset + ack->packet_length;

	hdr.command = GIP_CMD_ACKNOWLEDGE;
	hdr.options = client->id | GIP_OPT_INTERNAL;
	hdr.sequence = ack->sequence;
	hdr.packet_length = sizeof(pkt);

	pkt.command = ack->command;
	pkt.options = client->id | GIP_OPT_INTERNAL;
	pkt.length = cpu_to_le16(len);

	if ((ack->options & GIP_OPT_CHUNK) && buf)
		pkt.remaining = cpu_to_le16(buf->length - len);

	// gip_dbg(client, "%s: ACME(host) command=0x%02x, length=0x%04x\n",
	// 	__func__, pkt.command, len);

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

int gip_send_authenticate(struct gip_client *client, void *pkt, u32 len,
			  bool acknowledge)
{
	struct gip_header hdr = {};

	hdr.command = GIP_CMD_AUTHENTICATE;
	hdr.options = client->id | GIP_OPT_INTERNAL;
	hdr.packet_length = len;

	if (acknowledge)
		hdr.options |= GIP_OPT_ACKNOWLEDGE;

	return gip_send_pkt(client, &hdr, pkt);
}

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
			     enum gip_audio_format out,
			     bool chat)
{
	int err;

	/* special handling for the chat headset */
	if (chat)
		err = gip_set_audio_format_chat(client,
						GIP_AUD_FORMAT_CHAT_24KHZ);
	else
		err = gip_set_audio_format(client, in, out);

	if (err) {
		gip_err(client, "%s: set format failed: %d\n", __func__, err);
		return err;
	}

	client->audio_config_in.format = in;
	client->audio_config_out.format = out;

	return 0;
}
EXPORT_SYMBOL_GPL(gip_suggest_audio_format);

int gip_set_audio_volume(struct gip_client *client, u8 in, u8 chat, u8 out)
{
	struct gip_header hdr = {};
	struct gip_pkt_audio_volume pkt = {};

	hdr.command = GIP_CMD_AUDIO_CONTROL;
	hdr.options = client->id | GIP_OPT_INTERNAL;
	hdr.packet_length = sizeof(pkt);

	pkt.control.subcommand = GIP_AUD_CTRL_VOLUME;
	pkt.mute = GIP_AUD_VOLUME_UNMUTED;
	pkt.out = out;
	pkt.chat = chat;
	pkt.in = in;

	return gip_send_pkt(client, &hdr, &pkt);
}
EXPORT_SYMBOL_GPL(gip_set_audio_volume);

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
		gip_err(client, "%s: get buffer failed: %d\n", __func__, err);
		return err;
	}

	gip_copy_audio_samples(client, samples, buf.data);

	/* set actual length */
	buf.length = client->audio_config_out.packet_size *
		     adap->audio_packet_count;

	/* always fails on adapter removal */
	err = adap->ops->submit_buffer(adap, &buf);
	if (err)
		gip_dbg(client, "%s: submit buffer failed: %d\n",
			__func__, err);

	return err;
}
EXPORT_SYMBOL_GPL(gip_send_audio_samples);

bool gip_has_interface(struct gip_client *client, const guid_t *guid)
{
	int i;

	for (i = 0; i < client->interfaces->count; i++) {
		if (guid_equal((guid_t *)client->interfaces->data + i, guid))
			return true;
	}

	return false;
}
EXPORT_SYMBOL_GPL(gip_has_interface);

int gip_set_encryption_key(struct gip_client *client, u8 *key, int len)
{
	struct gip_adapter *adap = client->adapter;
	int err;

	if (!adap->ops->set_encryption_key) {
		gip_dbg(client, "%s: no callback, notifying driver.\n",
				__func__);
		if (client->drv->ops.authenticated)
			return client->drv->ops.authenticated(client);
		return 0;
	}

	err = adap->ops->set_encryption_key(adap, key, len);
	if (err) {
		gip_err(client, "%s: set key failed: %d\n", __func__, err);
		return err;
	}

	if (client->drv->ops.authenticated)
		client->drv->ops.authenticated(client);

	return 0;
}

int gip_enable_audio(struct gip_client *client)
{
	struct gip_adapter *adap = client->adapter;
	int err;

	if (!adap->ops->enable_audio)
		return 0;

	err = adap->ops->enable_audio(adap);
	if (err)
		gip_err(client, "%s: enable failed: %d\n", __func__, err);

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
		gip_err(client, "%s: init failed: %d\n", __func__, err);

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
		gip_err(client, "%s: init failed: %d\n", __func__, err);

	return err;
}
EXPORT_SYMBOL_GPL(gip_init_audio_out);

int gip_init_extra_data(struct gip_client *client)
{
	struct gip_header hdr = {
		.command = 0x4d, // ???
		.options = GIP_OPT_ACKNOWLEDGE, // Because 4
		.sequence = 1,
		.packet_length = 2,
	};
	u8 packet_data[] = { 0x07, 0x00 };

	return gip_send_pkt(client, &hdr, &packet_data);
}
EXPORT_SYMBOL_GPL(gip_init_extra_data);

void gip_disable_audio(struct gip_client *client)
{
	struct gip_adapter *adap = client->adapter;
	int err;

	if (!adap->ops->disable_audio)
		return;

	/* always fails on adapter removal */
	err = adap->ops->disable_audio(adap);
	if (err)
		gip_dbg(client, "%s: disable failed: %d\n", __func__, err);
}
EXPORT_SYMBOL_GPL(gip_disable_audio);

static int gip_make_audio_config(struct gip_client *client,
				 struct gip_audio_config *cfg)
{
	struct gip_header hdr = {};

	switch (cfg->format) {
	case GIP_AUD_FORMAT_16KHZ_MONO:
		cfg->channels = 1;
		cfg->sample_rate = 16000;
		break;
	case GIP_AUD_FORMAT_24KHZ_MONO:
		cfg->channels = 1;
		cfg->sample_rate = 24000;
		break;
	case GIP_AUD_FORMAT_48KHZ_STEREO:
		cfg->channels = 2;
		cfg->sample_rate = 48000;
		break;
	default:
		gip_err(client, "%s: unknown format: 0x%02x\n",
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

	gip_dbg(client, "%s: rate=%d/%d, buffer=%d\n", __func__,
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

static int gip_parse_client_commands(struct gip_client *client,
				     struct gip_pkt_identify *pkt,
				     u8 *data, u32 len)
{
	struct gip_info_element *cmds;
	struct gip_command_descriptor *desc;
	int i;

	cmds = gip_parse_info_element(data, len, pkt->client_commands_offset,
				      sizeof(*desc));
	if (IS_ERR(cmds)) {
		if (PTR_ERR(cmds) == -ENOTSUPP)
			return 0;

		gip_err(client, "%s: parse failed: %ld\n",
			__func__, PTR_ERR(cmds));
		return PTR_ERR(cmds);
	}

	for (i = 0; i < cmds->count; i++) {
		desc = (struct gip_command_descriptor *)cmds->data + i;
		gip_dbg(client,
			"%s: command=0x%02x, length=0x%02x, options=0x%02x\n",
			__func__, desc->command, desc->length, desc->options);
	}

	client->client_commands = cmds;

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
		gip_err(client, "%s: parse failed: %ld\n",
			__func__, PTR_ERR(vers));
		return PTR_ERR(vers);
	}

	for (i = 0; i < vers->count; i++) {
		ver = (struct gip_firmware_version *)vers->data + i;
		gip_dbg(client, "%s: version=%u.%u\n", __func__,
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

		gip_err(client, "%s: parse failed: %ld\n",
			__func__, PTR_ERR(fmts));
		return PTR_ERR(fmts);
	}

	gip_dbg(client, "%s: formats=%*phD\n", __func__,
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
		gip_err(client, "%s: parse out failed: %ld\n",
			__func__, PTR_ERR(caps));
		return PTR_ERR(caps);
	}

	gip_dbg(client, "%s: out=%*phD\n", __func__, caps->count, caps->data);
	client->capabilities_out = caps;

	caps = gip_parse_info_element(data, len,
				      pkt->capabilities_in_offset, 1);
	if (IS_ERR(caps)) {
		gip_err(client, "%s: parse in failed: %ld\n",
			__func__, PTR_ERR(caps));
		return PTR_ERR(caps);
	}

	gip_dbg(client, "%s: in=%*phD\n", __func__, caps->count, caps->data);
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

		str_len = le16_to_cpup((__le16 *)(data + off));
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

		gip_dbg(client, "%s: class=%s\n", __func__, str);
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
		gip_err(client, "%s: parse failed: %ld\n",
			__func__, PTR_ERR(intfs));
		return PTR_ERR(intfs);
	}

	for (i = 0; i < intfs->count; i++) {
		guid = (guid_t *)intfs->data + i;
		gip_dbg(client, "%s: guid=%pUb\n", __func__, guid);
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

		gip_err(client, "%s: parse failed: %ld\n",
			__func__, PTR_ERR(desc));
		return PTR_ERR(desc);
	}

	gip_dbg(client, "%s: length=0x%02x\n", __func__, desc->count);
	client->hid_descriptor = desc;

	return 0;
}

static int gip_send_remaining_chunks(struct gip_client *client, u32 offset)
{
	struct gip_chunk_buffer *buf = client->chunk_buf_in;
	struct gip_header hdr = buf->header;
	u32 len = buf->length - offset;
	int err;

	gip_dbg(client, "%s: sending chunk 0x%04x/0x%04x/0x%04x\n", __func__,
		len, hdr.chunk_offset, buf->length);

	int coalesce_count = GIP_PKT_COALESCE_COUNT;
	while (len && coalesce_count) {
		/* require acknowledgment for last chunk */
		if (len <= GIP_PKT_MAX_LENGTH)
			hdr.options |= GIP_OPT_ACKNOWLEDGE;

		hdr.packet_length = min_t(u32, len, GIP_PKT_MAX_LENGTH);
		hdr.chunk_offset = buf->length - len;

		err = gip_send_pkt_simple(client, &hdr,
					  buf->data + hdr.chunk_offset);
		if (err)
			return err;

		len -= hdr.packet_length;
		coalesce_count--;
	}

	return 0;
}

static int gip_handle_pkt_acknowledge(struct gip_client *client,
				      void *data, u32 len)
{
	struct gip_pkt_acknowledge *pkt = data;
	struct gip_chunk_buffer *buf = client->chunk_buf_in;
	struct gip_header hdr;

	if (len != sizeof(*pkt))
		return -EINVAL;

	if (!buf)
		return 0;

	gip_dbg(client, "%s: ACME(dev) cmd=0x%02x/0x%02x, len=0x%04x/0x%04x\n",
		__func__, pkt->command, buf->header.command,
		le16_to_cpu(pkt->length), buf->length);

	/* acknowledgment for different command */
	if (pkt->command != buf->header.command)
		return 0;

	/*
	 * offset comes from the device and may be malicious or invalid
	 * so sanitize value to prevent buffer overflow.
	 */
	if (le16_to_cpu(pkt->length) < buf->length)
		return gip_send_remaining_chunks(client, le16_to_cpu(pkt->length));

	gip_dbg(client, "%s: all chunks sent\n", __func__);

	/* empty chunk signals the completion of the transfer */
	hdr = buf->header;
	hdr.packet_length = 0;
	hdr.chunk_offset = buf->length;

	kfree(buf);
	client->chunk_buf_in = NULL;

	return gip_send_pkt_simple(client, &hdr, NULL);
}

static int gip_handle_pkt_announce(struct gip_client *client,
				   void *data, u32 len)
{
	struct gip_pkt_announce *pkt = data;
	struct gip_hardware *hw = &client->hardware;

	if (len != sizeof(*pkt))
		return -EINVAL;

	if (!hw->vendor && !hw->product && !hw->version) {
		hw->vendor = le16_to_cpu(pkt->vendor_id);
		hw->product = le16_to_cpu(pkt->product_id);
		hw->version = (le16_to_cpu(pkt->fw_version.major) << 8) |
			      le16_to_cpu(pkt->fw_version.minor);
	}

	gip_dbg(client, "%s: address=%pM, vendor=0x%04x, product=0x%04x\n",
		__func__, pkt->address, hw->vendor, hw->product);
	gip_dbg(client, "%s: firmware=%u.%u.%u.%u, hardware=%u.%u.%u.%u\n",
		__func__,
		le16_to_cpu(pkt->fw_version.major),
		le16_to_cpu(pkt->fw_version.minor),
		le16_to_cpu(pkt->fw_version.build),
		le16_to_cpu(pkt->fw_version.revision),
		le16_to_cpu(pkt->hw_version.major),
		le16_to_cpu(pkt->hw_version.minor),
		le16_to_cpu(pkt->hw_version.build),
		le16_to_cpu(pkt->hw_version.revision));

	return gip_request_identification(client);
}

static int gip_handle_pkt_status(struct gip_client *client,
				 void *data, u32 len)
{
	struct gip_pkt_status *pkt = data;
	int err = 0;
	u8 batt_type, batt_lvl;

	/* some devices occasionally send larger status packets */
	if (len < sizeof(*pkt))
		return -EINVAL;

	if (!(pkt->status & GIP_STATUS_CONNECTED)) {
		gip_dbg(client, "%s: disconnected\n", __func__);
		gip_remove_client(client);
		return 0;
	}

	batt_type = FIELD_GET(GIP_BATT_TYPE, pkt->status);
	batt_lvl = FIELD_GET(GIP_BATT_LEVEL, pkt->status);

	if (down_trylock(&client->drv_lock))
		return -EBUSY;

	if (client->drv && client->drv->ops.battery)
		err = client->drv->ops.battery(client, batt_type, batt_lvl);

	up(&client->drv_lock);

	return err;
}

static int gip_handle_pkt_identify(struct gip_client *client,
				   void *data, u32 len)
{
	struct gip_pkt_identify *pkt = data;
	int err;

	if (len < sizeof(*pkt))
		return -EINVAL;

	if (client->classes) {
		gip_warn(client, "%s: already identified\n", __func__);
		return 0;
	}

	/* skip unknown header */
	data += sizeof(pkt->unknown);
	len -= sizeof(pkt->unknown);

	err = gip_parse_client_commands(client, pkt, data, len);
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

	gip_add_client(client);

	return 0;

err_free_info:
	gip_free_client_info(client);

	return err;
}

static int gip_handle_pkt_authenticate(struct gip_client *client,
				       void *data, u32 len)
{
	int err = 0;

	if (down_trylock(&client->drv_lock))
		return -EBUSY;

	if (client->drv && client->drv->ops.authenticate)
		err = client->drv->ops.authenticate(client, data, len);

	up(&client->drv_lock);

	return err;
}

static int gip_handle_pkt_virtual_key(struct gip_client *client,
				      void *data, u32 len)
{
	struct gip_pkt_virtual_key *pkt = data;
	int err = 0;

	if (len != sizeof(*pkt))
		return -EINVAL;

	if (pkt->key != GIP_VKEY_LEFT_WIN)
		return -EINVAL;

	if (down_trylock(&client->drv_lock))
		return -EBUSY;

	if (client->drv && client->drv->ops.guide_button)
		err = client->drv->ops.guide_button(client, pkt->down);

	up(&client->drv_lock);

	return err;
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
	if (pkt->in_out != GIP_AUD_FORMAT_CHAT_24KHZ ||
	    in->buffer_size || out->buffer_size)
		return -EPROTO;

	err = gip_make_audio_config(client, in);
	if (err)
		return err;

	err = gip_make_audio_config(client, out);
	if (err)
		return err;

	if (down_trylock(&client->drv_lock))
		return -EBUSY;

	if (client->drv && client->drv->ops.audio_ready)
		err = client->drv->ops.audio_ready(client);

	up(&client->drv_lock);

	return err;
}

static int gip_handle_pkt_audio_volume_chat(struct gip_client *client,
					    void *data, u32 len)
{
	struct gip_pkt_audio_volume_chat *pkt = data;
	int err = 0;

	if (len != sizeof(*pkt))
		return -EINVAL;

	if (down_trylock(&client->drv_lock))
		return -EBUSY;

	if (client->drv && client->drv->ops.audio_volume)
		err = client->drv->ops.audio_volume(client, pkt->in, pkt->out);

	up(&client->drv_lock);

	return err;
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
	if (in->buffer_size || out->buffer_size)
		return -EPROTO;

	/* client rejected format, accept new format */
	if (pkt->in != in->format || pkt->out != out->format) {
		gip_warn(client, "%s: rejected: 0x%02x/0x%02x\n",
			 __func__, in->format, out->format);
		return gip_suggest_audio_format(client, pkt->in, pkt->out,
						false);
	}

	err = gip_make_audio_config(client, in);
	if (err)
		return err;

	err = gip_make_audio_config(client, out);
	if (err)
		return err;

	if (down_trylock(&client->drv_lock))
		return -EBUSY;

	if (client->drv && client->drv->ops.audio_ready)
		err = client->drv->ops.audio_ready(client);

	up(&client->drv_lock);

	return err;
}

static int gip_handle_pkt_audio_volume(struct gip_client *client,
				       void *data, u32 len)
{
	struct gip_pkt_audio_volume *pkt = data;
	int err = 0;

	if (len != sizeof(*pkt))
		return -EINVAL;

	if (down_trylock(&client->drv_lock))
		return -EBUSY;

	if (client->drv && client->drv->ops.audio_volume)
		err = client->drv->ops.audio_volume(client, pkt->in, pkt->out);

	up(&client->drv_lock);

	return err;
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

	gip_err(client, "%s: unknown subcommand: 0x%02x\n",
		__func__, pkt->subcommand);

	return -EPROTO;
}

static int gip_handle_pkt_hid_report(struct gip_client *client,
				     void *data, u32 len)
{
	int err = 0;

	if (down_trylock(&client->drv_lock))
		return -EBUSY;

	if (client->drv && client->drv->ops.hid_report)
		err = client->drv->ops.hid_report(client, data, len);

	up(&client->drv_lock);

	return err;
}

static int gip_handle_pkt_input(struct gip_client *client,
				void *data, u32 len)
{
	int err = 0;

	if (down_trylock(&client->drv_lock))
		return -EBUSY;

	if (client->drv && client->drv->ops.input)
		err = client->drv->ops.input(client, data, len);

	up(&client->drv_lock);

	return err;
}

static int gip_handle_pkt_audio_samples(struct gip_client *client,
					void *data, u32 len)
{
	struct gip_pkt_audio_samples *pkt = data;
	int err = 0;

	if (len < sizeof(*pkt))
		return -EINVAL;

	if (down_trylock(&client->drv_lock))
		return -EBUSY;

	if (client->drv && client->drv->ops.audio_samples)
		err = client->drv->ops.audio_samples(client, pkt->samples,
						     len - sizeof(*pkt));

	up(&client->drv_lock);

	return err;
}

static int gip_handle_pkt_firmware(struct gip_client *client, void *data,
				   u32 len)
{
	int err = 0;

	if (down_trylock(&client->drv_lock))
		return -EBUSY;

	if (client->drv && client->drv->ops.firmware)
		err = client->drv->ops.firmware(client, data, len);

	up(&client->drv_lock);

	return err;
}

static int gip_dispatch_pkt(struct gip_client *client,
			    struct gip_header *hdr, void *data, u32 len)
{
	if (hdr->options & GIP_OPT_INTERNAL) {
		switch (hdr->command) {
		case GIP_CMD_ACKNOWLEDGE:
			return gip_handle_pkt_acknowledge(client, data, len);
		case GIP_CMD_ANNOUNCE:
			return gip_handle_pkt_announce(client, data, len);
		case GIP_CMD_STATUS:
			return gip_handle_pkt_status(client, data, len);
		case GIP_CMD_IDENTIFY:
			return gip_handle_pkt_identify(client, data, len);
		case GIP_CMD_AUTHENTICATE:
			return gip_handle_pkt_authenticate(client, data, len);
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
	case GIP_CMD_FIRMWARE:
		return gip_handle_pkt_firmware(client, data, len);
	default:
		pr_debug("%s: Unknown hdr command", __func__);
	}

	return 0;
}

static int gip_process_pkt_chunked(struct gip_client *client,
				   struct gip_header *hdr, void *data)
{
	struct gip_chunk_buffer *buf = client->chunk_buf_out;
	int err;
	u32 len;

	gip_dbg(client, "%s: flags=[%s %s %s], offset=0x%04x, length=0x%04x\n",
		__func__,
		hdr->options & GIP_OPT_CHUNK_START? "Ini":"...",
		hdr->options & GIP_OPT_ACKNOWLEDGE? "Ack":"...",
		hdr->options & GIP_OPT_INTERNAL? "Sys":"...",
		hdr->chunk_offset, hdr->packet_length);

	if (!buf) {
		/* older gamepads occasionally send spurious completions */
		if (!hdr->packet_length)
			return 0;

		gip_err(client, "%s: buffer not allocated\n", __func__);
		return -EPROTO;
	}

	if (hdr->command != buf->header.command) {
		gip_err(client, "%s: conflicting packet\n", __func__);
		return -EALREADY;
	}

	len = hdr->chunk_offset + hdr->packet_length;
	if (buf->length < len) {
		gip_err(client, "%s: buffer too small\n", __func__);
		return -EINVAL;
	}

	if (hdr->packet_length) {
		/*
		 * acknowledge last non-empty chunked packet even when
		 * not asked in current chunk header
		 */
		bool last_chunk = (buf->length == len);
		if ((hdr->options & GIP_OPT_ACKNOWLEDGE) || last_chunk) {
			err = gip_acknowledge_pkt(client, hdr);
			if (err)
				return err;
		}
		memcpy(buf->data + hdr->chunk_offset, data, hdr->packet_length);
		return 0;
	}

	/* empty chunk signals the completion of the transfer */
	err = gip_dispatch_pkt(client, hdr, buf->data, buf->length);

	kfree(buf);
	client->chunk_buf_out = NULL;

	return err;
}

static int gip_process_pkt(struct gip_client *client,
			   struct gip_header *hdr, void *data)
{
	int err;

	if (hdr->options & GIP_OPT_CHUNK_START) {
		err = gip_init_chunk_buffer(client, hdr,
					    &client->chunk_buf_out, true);
		if (err)
			return err;

		hdr->chunk_offset = 0;
	}

	if (hdr->options & GIP_OPT_CHUNK)
		return gip_process_pkt_chunked(client, hdr, data);

	if (hdr->options & GIP_OPT_ACKNOWLEDGE) {
		err = gip_acknowledge_pkt(client, hdr);
		if (err)
			return err;
	}

	return gip_dispatch_pkt(client, hdr, data, hdr->packet_length);
}

int gip_process_buffer(struct gip_adapter *adap, void *data, int len)
{
	struct gip_header hdr;
	struct gip_client *client;
	int hdr_len, err;

	while (len > GIP_HDR_MIN_LENGTH) {
		hdr_len = gip_decode_header(&hdr, data, len);
		if (len < hdr_len + hdr.packet_length)
			return -EINVAL;

		client = gip_get_client(adap, hdr.options & GIP_HDR_CLIENT_ID);
		if (IS_ERR(client))
			return PTR_ERR(client);

		err = gip_process_pkt(client, &hdr, data + hdr_len);
		if (err)
			return err;

		data += hdr_len + hdr.packet_length;
		len -= hdr_len + hdr.packet_length;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(gip_process_buffer);
