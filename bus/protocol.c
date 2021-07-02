// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2021 Severin von Wnuck <severinvonw@outlook.de>
 */

#include <linux/slab.h>
#include <linux/bitfield.h>
#include <linux/uuid.h>

#include "bus.h"

/* product ID for the chat headset */
#define GIP_PID_CHAT_HEADSET 0x0111

#define GIP_HEADER_CLIENT_ID GENMASK(3, 0)
#define GIP_HEADER_LENGTH GENMASK(6, 0)
#define GIP_HEADER_EXTENDED BIT(7)

#define GIP_BATT_LEVEL GENMASK(1, 0)
#define GIP_BATT_TYPE GENMASK(3, 2)
#define GIP_STATUS_CONNECTED BIT(7)

#define GIP_AUD_LENGTH_EXTRA GENMASK(3, 0)

enum gip_option {
	GIP_OPT_ACKNOWLEDGE = BIT(4),
	GIP_OPT_REQUEST = BIT(5),
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
	__le16 unknown_offset1;
	__le16 unknown_offset2;
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

struct gip_pkt_audio_header_ext {
	u8 unknown[2];
} __packed;

struct gip_chunk {
	int offset;
	void *data;
	int length;
};

static int gip_send_pkt(struct gip_client *client,
		struct gip_header *header, void *data, int len)
{
	struct gip_adapter *adap = client->adapter;
	struct gip_adapter_buffer buf = {};
	int err;
	unsigned long flags;

	buf.type = GIP_BUF_DATA;

	spin_lock_irqsave(&adap->send_lock, flags);

	/* sequence number is always greater than zero */
	while (!header->sequence)
		header->sequence = adap->data_sequence++;

	err = adap->ops->get_buffer(adap, &buf);
	if (err) {
		dev_err(&client->dev, "%s: get buffer failed: %d\n",
				__func__, err);
		goto err_unlock;
	}

	/* check available space */
	if (buf.length < sizeof(*header) + len) {
		dev_err(&client->dev, "%s: buffer too small\n", __func__);
		err = -ENOSPC;
		goto err_unlock;
	}

	memcpy(buf.data, header, sizeof(*header));
	if (data)
		memcpy(buf.data + sizeof(*header), data, len);

	/* set actual length */
	buf.length = sizeof(*header) + len;

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
		struct gip_header *ack, u16 len, u16 remaining)
{
	struct gip_header header = {};
	struct gip_pkt_acknowledge pkt = {};

	header.command = GIP_CMD_ACKNOWLEDGE;
	header.options = client->id | GIP_OPT_REQUEST;
	header.sequence = ack->sequence;
	header.length = sizeof(pkt);

	pkt.inner.command = ack->command;
	pkt.inner.options = client->id | GIP_OPT_REQUEST;
	pkt.inner.sequence = len;
	pkt.inner.length = len >> 8;

	/* only required for the start chunk */
	pkt.remaining = cpu_to_le16(remaining);

	return gip_send_pkt(client, &header, &pkt, sizeof(pkt));
}

static int gip_request_identification(struct gip_client *client)
{
	struct gip_header header = {};

	header.command = GIP_CMD_IDENTIFY;
	header.options = client->id | GIP_OPT_REQUEST;

	return gip_send_pkt(client, &header, NULL, 0);
}

int gip_set_power_mode(struct gip_client *client, enum gip_power_mode mode)
{
	struct gip_header header = {};
	struct gip_pkt_power pkt = {};

	header.command = GIP_CMD_POWER;
	header.options = client->id | GIP_OPT_REQUEST;
	header.length = sizeof(pkt);

	pkt.mode = mode;

	return gip_send_pkt(client, &header, &pkt, sizeof(pkt));
}
EXPORT_SYMBOL_GPL(gip_set_power_mode);

int gip_complete_authentication(struct gip_client *client)
{
	struct gip_header header = {};
	struct gip_pkt_authenticate pkt = {};

	header.command = GIP_CMD_AUTHENTICATE;
	header.options = client->id | GIP_OPT_REQUEST;
	header.length = sizeof(pkt);

	pkt.unknown1 = 0x01;

	return gip_send_pkt(client, &header, &pkt, sizeof(pkt));
}
EXPORT_SYMBOL_GPL(gip_complete_authentication);

static int gip_set_audio_format_chat(struct gip_client *client,
		enum gip_audio_format_chat in_out)
{
	struct gip_header header = {};
	struct gip_pkt_audio_format_chat pkt = {};

	header.command = GIP_CMD_AUDIO_CONTROL;
	header.options = client->id | GIP_OPT_REQUEST;
	header.length = sizeof(pkt);

	pkt.control.subcommand = GIP_AUD_CTRL_FORMAT_CHAT;
	pkt.in_out = in_out;

	return gip_send_pkt(client, &header, &pkt, sizeof(pkt));
}

static int gip_set_audio_format(struct gip_client *client,
		enum gip_audio_format in, enum gip_audio_format out)
{
	struct gip_header header = {};
	struct gip_pkt_audio_format pkt = {};

	header.command = GIP_CMD_AUDIO_CONTROL;
	header.options = client->id | GIP_OPT_REQUEST;
	header.length = sizeof(pkt);

	pkt.control.subcommand = GIP_AUD_CTRL_FORMAT;
	pkt.in = in;
	pkt.out = out;

	return gip_send_pkt(client, &header, &pkt, sizeof(pkt));
}

int gip_suggest_audio_format(struct gip_client *client,
		enum gip_audio_format in, enum gip_audio_format out)
{
	int err;

	/* special handling for the chat headset */
	if (client->hardware.product == GIP_PID_CHAT_HEADSET)
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
	struct gip_header header = {};
	struct gip_pkt_audio_volume pkt = {};

	header.command = GIP_CMD_AUDIO_CONTROL;
	header.options = client->id | GIP_OPT_REQUEST;
	header.length = sizeof(pkt);

	pkt.control.subcommand = GIP_AUD_CTRL_VOLUME;
	pkt.mute = GIP_AUD_VOLUME_UNMUTED;
	pkt.out = out;
	pkt.in = in;

	return gip_send_pkt(client, &header, &pkt, sizeof(pkt));
}

int gip_fix_audio_volume(struct gip_client *client)
{
	/* chat headsets have buttons to adjust the hardware volume */
	if (client->hardware.product == GIP_PID_CHAT_HEADSET)
		return 0;

	/* set hardware volume to maximum */
	return gip_set_audio_volume(client, 100, 100);
}
EXPORT_SYMBOL_GPL(gip_fix_audio_volume);

int gip_send_rumble(struct gip_client *client, void *pkt, u8 len)
{
	struct gip_header header = {};

	header.command = GIP_CMD_RUMBLE;
	header.options = client->id;
	header.length = len;

	return gip_send_pkt(client, &header, pkt, len);
}
EXPORT_SYMBOL_GPL(gip_send_rumble);

int gip_set_led_mode(struct gip_client *client,
		enum gip_led_mode mode, u8 brightness)
{
	struct gip_header header = {};
	struct gip_pkt_led pkt = {};

	header.command = GIP_CMD_LED;
	header.options = client->id | GIP_OPT_REQUEST;
	header.length = sizeof(pkt);

	pkt.mode = mode;
	pkt.brightness = brightness;

	return gip_send_pkt(client, &header, &pkt, sizeof(pkt));
}
EXPORT_SYMBOL_GPL(gip_set_led_mode);

static void gip_copy_audio_samples(struct gip_client *client,
		void *samples, void *buf)
{
	struct gip_audio_config *cfg = &client->audio_config_out;
	struct gip_header header = {};
	struct gip_pkt_audio_header pkt = {};
	void *src, *dest;
	int i;

	/* packet length does not include audio header size */
	header.command = GIP_CMD_AUDIO_SAMPLES;
	header.options = client->id | GIP_OPT_REQUEST;
	header.length = cfg->fragment_size;

	if (cfg->fragment_size > GIP_HEADER_LENGTH) {
		header.length |= GIP_HEADER_EXTENDED;
		pkt.length_extra = GIP_HEADER_EXTENDED |
				(cfg->fragment_size >> 7);
	}

	for (i = 0; i < client->adapter->audio_packet_count; i++) {
		src = samples + i * cfg->fragment_size;
		dest = buf + i * cfg->packet_size;

		/* sequence number is always greater than zero */
		do {
			header.sequence = client->adapter->audio_sequence++;
		} while (!header.sequence);

		memcpy(dest, &header, sizeof(header));

		if (cfg->fragment_size > GIP_HEADER_LENGTH) {
			memcpy(dest + sizeof(header), &pkt, sizeof(pkt));
			memcpy(dest + sizeof(header) + sizeof(pkt),
					src, cfg->fragment_size);
		} else {
			memcpy(dest + sizeof(header), src, cfg->fragment_size);
		}
	}
}

int gip_send_audio_samples(struct gip_client *client, void *samples)
{
	struct gip_adapter *adap = client->adapter;
	struct gip_adapter_buffer buf = {};
	int err;

	buf.type = GIP_BUF_AUDIO;

	err = adap->ops->get_buffer(adap, &buf);
	if (err) {
		dev_err(&client->dev, "%s: get buffer failed: %d\n",
				__func__, err);
		return err;
	}

	gip_copy_audio_samples(client, samples, buf.data);

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

	err = adap->ops->init_audio_in(adap);
	if (err)
		dev_err(&client->dev, "%s: init failed: %d\n",
				__func__, err);

	return err;
}
EXPORT_SYMBOL_GPL(gip_init_audio_in);

int gip_init_audio_out(struct gip_client *client)
{
	struct gip_adapter *adap = client->adapter;
	int err;

	err = adap->ops->init_audio_out(adap,
			client->audio_config_out.packet_size);
	if (err)
		dev_err(&client->dev, "%s: init failed: %d\n",
				__func__, err);

	return err;
}
EXPORT_SYMBOL_GPL(gip_init_audio_out);

void gip_disable_audio(struct gip_client *client)
{
	struct gip_adapter *adap = client->adapter;
	int err;

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

	cfg->valid = true;
	cfg->buffer_size = cfg->sample_rate * cfg->channels *
			sizeof(s16) * GIP_AUDIO_INTERVAL / MSEC_PER_SEC;
	cfg->fragment_size = cfg->buffer_size / client->adapter->audio_packet_count;
	cfg->packet_size = cfg->fragment_size + sizeof(struct gip_header);

	if (cfg->fragment_size > GIP_HEADER_LENGTH)
		cfg->packet_size += sizeof(struct gip_pkt_audio_header);

	dev_dbg(&client->dev, "%s: rate=%d/%d, buffer=%d\n", __func__,
			cfg->sample_rate, cfg->channels, cfg->buffer_size);

	return 0;
}

static struct gip_info_element *gip_parse_info_element(u8 *data, int len,
		__le16 offset, int item_length)
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

	elem->length = total;
	memcpy(elem->data, data + off, total);

	return elem;
}

static int gip_parse_audio_formats(struct gip_client *client,
		struct gip_pkt_identify *pkt, u8 *data, int len)
{
	struct gip_info_element *fmts;

	fmts = gip_parse_info_element(data, len,
			pkt->audio_formats_offset, sizeof(u8) * 2);
	if (IS_ERR(fmts)) {
		if (PTR_ERR(fmts) == -ENOTSUPP)
			return 0;

		dev_err(&client->dev, "%s: parse failed: %ld\n",
				__func__, PTR_ERR(fmts));
		return PTR_ERR(fmts);
	}

	dev_dbg(&client->dev, "%s: formats=%*phD\n", __func__,
			fmts->length, fmts->data);
	client->audio_formats = fmts;

	return 0;
}

static int gip_parse_capabilities(struct gip_client *client,
		struct gip_pkt_identify *pkt, u8 *data, int len)
{
	struct gip_info_element *caps;

	caps = gip_parse_info_element(data, len,
			pkt->capabilities_out_offset, sizeof(u8));
	if (IS_ERR(caps)) {
		dev_err(&client->dev, "%s: parse out failed: %ld\n",
				__func__, PTR_ERR(caps));
		return PTR_ERR(caps);
	}

	dev_dbg(&client->dev, "%s: out=%*phD\n", __func__,
			caps->length, caps->data);
	client->capabilities_out = caps;

	caps = gip_parse_info_element(data, len,
			pkt->capabilities_in_offset, sizeof(u8));
	if (IS_ERR(caps)) {
		dev_err(&client->dev, "%s: parse in failed: %ld\n",
				__func__, PTR_ERR(caps));
		return PTR_ERR(caps);
	}

	dev_dbg(&client->dev, "%s: in=%*phD\n", __func__,
			caps->length, caps->data);
	client->capabilities_in = caps;

	return 0;
}

static int gip_parse_classes(struct gip_client *client,
		struct gip_pkt_identify *pkt, u8 *data, int len)
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

	classes = kzalloc(sizeof(*classes) + sizeof(char *) * count, GFP_ATOMIC);
	if (!classes)
		return -ENOMEM;

	client->classes = classes;

	while (classes->count < count) {
		if (len < off + sizeof(str_len))
			return -EINVAL;

		str_len = le16_to_cpup((u16 *)(data + off));
		if (!str_len || len < off + sizeof(str_len) + str_len)
			return -EINVAL;

		/* null-terminated string */
		str = kzalloc(str_len + 1, GFP_ATOMIC);
		if (!str)
			return -ENOMEM;

		memcpy(str, data + off + sizeof(str_len), str_len);
		classes->strings[classes->count] = str;
		classes->count++;
		off += str_len;

		dev_dbg(&client->dev, "%s: class=%s\n", __func__, str);
	}

	return 0;
}

static int gip_parse_interfaces(struct gip_client *client,
		struct gip_pkt_identify *pkt, u8 *data, int len)
{
	struct gip_info_element *intfs;
	int i;

	intfs = gip_parse_info_element(data, len,
			pkt->interfaces_offset, UUID_SIZE);
	if (IS_ERR(intfs)) {
		dev_err(&client->dev, "%s: parse failed: %ld\n",
				__func__, PTR_ERR(intfs));
		return PTR_ERR(intfs);
	}

	for (i = 0; i < intfs->length; i += UUID_SIZE)
		dev_dbg(&client->dev, "%s: guid=%pU\n", __func__,
				intfs->data + i);

	client->interfaces = intfs;

	return 0;
}

static int gip_parse_hid_descriptor(struct gip_client *client,
		struct gip_pkt_identify *pkt, u8 *data, int len)
{
	struct gip_info_element *desc;

	desc = gip_parse_info_element(data, len,
			pkt->hid_descriptor_offset, sizeof(u8));
	if (IS_ERR(desc)) {
		if (PTR_ERR(desc) == -ENOTSUPP)
			return 0;

		dev_err(&client->dev, "%s: parse failed: %ld\n",
				__func__, PTR_ERR(desc));
		return PTR_ERR(desc);
	}

	dev_dbg(&client->dev, "%s: length=0x%02x\n", __func__, desc->length);
	client->hid_descriptor = desc;

	return 0;
}

static int gip_handle_pkt_announce(struct gip_client *client,
		struct gip_header *header, void *data, int len)
{
	struct gip_pkt_announce *pkt = data;
	struct gip_hardware *hw = &client->hardware;

	if (len != header->length || len != sizeof(*pkt))
		return -EINVAL;

	if (atomic_read(&client->state) != GIP_CL_CONNECTED) {
		dev_warn(&client->dev, "%s: invalid state\n", __func__);
		return 0;
	}

	hw->vendor = le16_to_cpu(pkt->vendor_id);
	hw->product = le16_to_cpu(pkt->product_id);
	hw->version = (le16_to_cpu(pkt->fw_version.major) << 8) |
			le16_to_cpu(pkt->fw_version.minor);

	dev_dbg(&client->dev, "%s: address=%pM, vendor=0x%04x, product=0x%04x\n",
			__func__, pkt->address, hw->vendor, hw->product);
	dev_dbg(&client->dev, "%s: firmware=%u.%u.%u.%u, hardware=%u.%u.%u.%u\n",
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
		struct gip_header *header, void *data, int len)
{
	struct gip_pkt_status *pkt = data;

	if (len != header->length || len != sizeof(*pkt))
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
		struct gip_header *header, void *data, int len)
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
		struct gip_header *header, void *data, int len)
{
	struct gip_pkt_guide_button *pkt = data;

	if (len != header->length || len != sizeof(*pkt))
		return -EINVAL;

	if (!client->drv || !client->drv->ops.guide_button)
		return 0;

	return client->drv->ops.guide_button(client, pkt->pressed);
}

static int gip_handle_pkt_audio_format_chat(struct gip_client *client,
		struct gip_header *header, void *data, int len)
{
	struct gip_pkt_audio_format_chat *pkt = data;
	struct gip_audio_config *in = &client->audio_config_in;
	struct gip_audio_config *out = &client->audio_config_out;
	int err;

	if (len != header->length || len != sizeof(*pkt))
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
		struct gip_header *header, void *data, int len)
{
	struct gip_pkt_audio_volume_chat *pkt = data;

	if (len != header->length || len != sizeof(*pkt))
		return -EINVAL;

	if (!client->drv || !client->drv->ops.audio_volume)
		return 0;

	return client->drv->ops.audio_volume(client, pkt->in, pkt->out);
}

static int gip_handle_pkt_audio_format(struct gip_client *client,
		struct gip_header *header, void *data, int len)
{
	struct gip_pkt_audio_format *pkt = data;
	struct gip_audio_config *in = &client->audio_config_in;
	struct gip_audio_config *out = &client->audio_config_out;
	int err;

	if (len != header->length || len != sizeof(*pkt))
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
		struct gip_header *header, void *data, int len)
{
	struct gip_pkt_audio_volume *pkt = data;

	if (len != header->length || len != sizeof(*pkt))
		return -EINVAL;

	if (!client->drv || !client->drv->ops.audio_volume)
		return 0;

	return client->drv->ops.audio_volume(client, pkt->in, pkt->out);
}

static int gip_handle_pkt_audio_control(struct gip_client *client,
		struct gip_header *header, void *data, int len)
{
	struct gip_pkt_audio_control *pkt = data;

	if (len < sizeof(*pkt))
		return -EINVAL;

	switch (pkt->subcommand) {
	case GIP_AUD_CTRL_FORMAT_CHAT:
		return gip_handle_pkt_audio_format_chat(client, header, data, len);
	case GIP_AUD_CTRL_VOLUME_CHAT:
		return gip_handle_pkt_audio_volume_chat(client, header, data, len);
	case GIP_AUD_CTRL_FORMAT:
		return gip_handle_pkt_audio_format(client, header, data, len);
	case GIP_AUD_CTRL_VOLUME:
		return gip_handle_pkt_audio_volume(client, header, data, len);
	}

	dev_err(&client->dev, "%s: unknown subcommand: 0x%02x\n",
			__func__, pkt->subcommand);

	return -EPROTO;
}

static int gip_handle_pkt_hid_report(struct gip_client *client,
		struct gip_header *header, void *data, int len)
{
	if (len != header->length)
		return -EINVAL;

	if (!client->drv || !client->drv->ops.hid_report)
		return 0;

	return client->drv->ops.hid_report(client, data, len);
}

static int gip_handle_pkt_input(struct gip_client *client,
		struct gip_header *header, void *data, int len)
{
	if (len != header->length)
		return -EINVAL;

	if (!client->drv || !client->drv->ops.input)
		return 0;

	return client->drv->ops.input(client, data, len);
}

static int gip_handle_pkt_audio_samples(struct gip_client *client,
		struct gip_header *header, void *data, int len)
{
	struct gip_pkt_audio_header *pkt = data;
	struct gip_pkt_audio_header_ext *ext = data + sizeof(*pkt);
	int total = header->length & GIP_HEADER_LENGTH;

	if (len < sizeof(*pkt))
		return -EINVAL;

	/* extended audio headers are used by wireless clients */
	if (header->options & GIP_HEADER_EXTENDED) {
		total |= (pkt->length_extra & GIP_AUD_LENGTH_EXTRA) << 7;
		if (total < sizeof(*pkt) + sizeof(*ext) || len < total)
			return -EINVAL;

		data += sizeof(*pkt) + sizeof(*ext);
		total -= sizeof(*pkt) + sizeof(*ext);
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

static int gip_handle_pkt(struct gip_client *client,
		struct gip_header *header, void *data, int len)
{
	switch (header->command) {
	case GIP_CMD_ACKNOWLEDGE:
		/* ignore acknowledgements */
		return 0;
	case GIP_CMD_ANNOUNCE:
		return gip_handle_pkt_announce(client, header, data, len);
	case GIP_CMD_STATUS:
		return gip_handle_pkt_status(client, header, data, len);
	case GIP_CMD_IDENTIFY:
		return gip_handle_pkt_identify(client, header, data, len);
	case GIP_CMD_GUIDE_BUTTON:
		return gip_handle_pkt_guide_button(client, header, data, len);
	case GIP_CMD_AUDIO_CONTROL:
		return gip_handle_pkt_audio_control(client, header, data, len);
	case GIP_CMD_HID_REPORT:
		return gip_handle_pkt_hid_report(client, header, data, len);
	case GIP_CMD_INPUT:
		return gip_handle_pkt_input(client, header, data, len);
	case GIP_CMD_AUDIO_SAMPLES:
		return gip_handle_pkt_audio_samples(client, header, data, len);
	}

	dev_warn(&client->dev, "%s: unknown command: 0x%02x\n",
			__func__, header->command);

	return 0;
}

static int gip_parse_chunk(struct gip_client *client,
		void *data, int len, struct gip_chunk *chunk)
{
	struct gip_header *header = data;
	struct gip_chunk_header *chunk_header = data + sizeof(*header);

	if (len < sizeof(*header) + sizeof(*chunk_header)) {
		dev_err(&client->dev, "%s: invalid length\n", __func__);
		return -EINVAL;
	}

	if (header->length & GIP_HEADER_EXTENDED)
		chunk->offset = chunk_header->offset;
	else
		chunk->offset = (chunk_header->offset_extra & 0x7f) |
				(chunk_header->offset << 7);

	chunk->data = data + sizeof(*header) + sizeof(*chunk_header);
	chunk->length = len - sizeof(*header) - sizeof(*chunk_header);

	if (chunk->length != (header->length & GIP_HEADER_LENGTH)) {
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

	/* last chunk is empty, offset is total length of all chunks */
	if (!chunk.length && chunk.offset == buf->length) {
		dev_dbg(&client->dev, "%s: buffer complete\n", __func__);
		buf->full = true;
	} else {
		memcpy(buf->data + chunk.offset, chunk.data, chunk.length);
	}

	return 0;
}

static int gip_process_chunk(struct gip_client *client, void *data, int len)
{
	struct gip_header *header = data;
	struct gip_chunk chunk = {};
	int err;

	err = gip_parse_chunk(client, data, len, &chunk);
	if (err)
		return err;

	if (header->options & GIP_OPT_CHUNK_START) {
		/* offset is total length of all chunks */
		err = gip_init_chunk_buffer(client, chunk.offset);
		if (err)
			return err;

		/* acknowledge with remaining length */
		err = gip_acknowledge_pkt(client, header, chunk.length,
				client->chunk_buf->length - chunk.length);
		if (err)
			return err;

		chunk.offset = 0;
	} else if (header->options & GIP_OPT_ACKNOWLEDGE) {
		/* acknowledge with total buffer length */
		err = gip_acknowledge_pkt(client, header,
				client->chunk_buf->length, 0);
		if (err)
			return err;
	}

	return gip_copy_chunk_data(client, chunk);
}

static int gip_process_pkt_coherent(struct gip_client *client,
		void *data, int len)
{
	struct gip_header *header = data;
	int err;

	if (header->options & GIP_OPT_ACKNOWLEDGE) {
		err = gip_acknowledge_pkt(client, header, header->length, 0);
		if (err)
			return err;
	}

	return gip_handle_pkt(client, header, data + sizeof(*header),
			len - sizeof(*header));
}

static int gip_process_pkt_chunked(struct gip_client *client,
		void *data, int len)
{
	struct gip_header *header = data;
	struct gip_chunk_buffer *buf;
	int err;

	err = gip_process_chunk(client, data, len);
	if (err)
		return err;

	/* all chunks have been received */
	buf = client->chunk_buf;
	if (buf->full) {
		err = gip_handle_pkt(client, header, buf->data, buf->length);

		kfree(buf);
		client->chunk_buf = NULL;
	}

	return err;
}

int gip_process_buffer(struct gip_adapter *adap, void *data, int len)
{
	struct gip_header *header = data;
	struct gip_client *client;
	u8 id = header->options & GIP_HEADER_CLIENT_ID;
	int err = 0;
	unsigned long flags;

	if (len < sizeof(*header)) {
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

	if (header->options & GIP_OPT_CHUNK)
		err = gip_process_pkt_chunked(client, data, len);
	else
		err = gip_process_pkt_coherent(client, data, len);

	if (err) {
		dev_err(&adap->dev, "%s: process packet failed: %d\n",
				__func__, err);
		print_hex_dump_debug("", DUMP_PREFIX_NONE, 16, 1,
				data, len, false);
	}

err_unlock:
	spin_unlock_irqrestore(&client->lock, flags);
	gip_put_client(client);

	return err;
}
EXPORT_SYMBOL_GPL(gip_process_buffer);
