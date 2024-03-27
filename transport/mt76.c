// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2021 Severin von Wnuck-Lipinski <severinvonw@outlook.de>
 */

#include <linux/slab.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/usb.h>
#include <linux/firmware.h>
#include <linux/ieee80211.h>

#include "mt76.h"

/* bulk transfer timeout in ms */
#define XONE_MT_USB_TIMEOUT 1000

#define XONE_MT_POLL_RETRIES 50

#define XONE_MT_RF_PATCH 0x0130
#define XONE_MT_FW_LOAD_IVB 0x12
#define XONE_MT_FW_ILM_OFFSET 0x080000
#define XONE_MT_FW_DLM_OFFSET 0x110800
#define XONE_MT_FW_CHUNK_SIZE 0x3800

/* wireless channel bands */
#define XONE_MT_CH_2G_LOW 0x01
#define XONE_MT_CH_2G_MID 0x02
#define XONE_MT_CH_2G_HIGH 0x03
#define XONE_MT_CH_5G_LOW 0x01
#define XONE_MT_CH_5G_HIGH 0x02

#define XONE_MT_WCID_KEY_LEN 16

/* commands specific to the dongle's firmware */
enum xone_mt76_ms_command {
	XONE_MT_SET_MAC_ADDRESS = 0x00,
	XONE_MT_ADD_CLIENT = 0x01,
	XONE_MT_REMOVE_CLIENT = 0x02,
	XONE_MT_SET_IDLE_TIME = 0x05,
	XONE_MT_SET_CHAN_CANDIDATES = 0x07,
};

enum xone_mt76_wow_feature {
	XONE_MT_WOW_ENABLE = 0x01,
	XONE_MT_WOW_TRAFFIC = 0x03,
};

enum xone_mt76_wow_traffic {
	XONE_MT_WOW_TO_FIRMWARE = 0x00,
	XONE_MT_WOW_TO_HOST = 0x01,
};

struct xone_mt76_msg_load_cr {
	u8 mode;
	u8 temperature;
	u8 channel;
	u8 padding;
} __packed;

struct xone_mt76_msg_switch_channel {
	u8 channel;
	u8 padding1[3];
	__le16 tx_rx_setting;
	u8 padding2[10];
	u8 bandwidth;
	u8 tx_power;
	u8 scan;
	u8 unknown;
} __packed;

static u32 xone_mt76_read_register(struct xone_mt76 *mt, u32 addr)
{
	u8 req = MT_VEND_MULTI_READ;
	int ret;

	if (addr & MT_VEND_TYPE_CFG) {
		req = MT_VEND_READ_CFG;
		addr &= ~MT_VEND_TYPE_CFG;
	}

	ret = usb_control_msg(mt->udev, usb_rcvctrlpipe(mt->udev, 0), req,
			      USB_DIR_IN | USB_TYPE_VENDOR, addr >> 16, addr,
			      &mt->control_data, sizeof(mt->control_data),
			      XONE_MT_USB_TIMEOUT);
	if (ret != sizeof(mt->control_data))
		ret = -EREMOTEIO;

	if (ret < 0) {
		dev_err(mt->dev, "%s: control message failed: %d\n",
			__func__, ret);
		return 0;
	}

	return le32_to_cpu(mt->control_data);
}

static void xone_mt76_write_register(struct xone_mt76 *mt, u32 addr, u32 val)
{
	u8 req = MT_VEND_MULTI_WRITE;
	int ret;

	if (addr & MT_VEND_TYPE_CFG) {
		req = MT_VEND_WRITE_CFG;
		addr &= ~MT_VEND_TYPE_CFG;
	}

	mt->control_data = cpu_to_le32(val);

	ret = usb_control_msg(mt->udev, usb_sndctrlpipe(mt->udev, 0), req,
			      USB_DIR_OUT | USB_TYPE_VENDOR, addr >> 16, addr,
			      &mt->control_data, sizeof(mt->control_data),
			      XONE_MT_USB_TIMEOUT);
	if (ret != sizeof(mt->control_data))
		ret = -EREMOTEIO;

	if (ret < 0)
		dev_err(mt->dev, "%s: control message failed: %d\n",
			__func__, ret);
}

static int xone_mt76_load_ivb(struct xone_mt76 *mt)
{
	/* load interrupt vector block */
	return usb_control_msg(mt->udev, usb_sndctrlpipe(mt->udev, 0),
			       MT_VEND_DEV_MODE, USB_DIR_OUT | USB_TYPE_VENDOR,
			       XONE_MT_FW_LOAD_IVB, 0, NULL, 0,
			       XONE_MT_USB_TIMEOUT);
}

static bool xone_mt76_poll(struct xone_mt76 *mt, u32 offset, u32 mask, u32 val)
{
	int i;
	u32 reg;

	for (i = 0; i < XONE_MT_POLL_RETRIES; i++) {
		reg = xone_mt76_read_register(mt, offset);
		if ((reg & mask) == val)
			return true;

		usleep_range(10000, 20000);
	}

	return false;
}

static int xone_mt76_read_efuse(struct xone_mt76 *mt, u16 addr,
				void *data, int len)
{
	u32 ctrl, offset, val;
	int i, remaining;

	ctrl = xone_mt76_read_register(mt, MT_EFUSE_CTRL);
	ctrl &= ~(MT_EFUSE_CTRL_AIN | MT_EFUSE_CTRL_MODE);
	ctrl |= MT_EFUSE_CTRL_KICK;
	ctrl |= FIELD_PREP(MT_EFUSE_CTRL_AIN, addr & ~0x0f);
	ctrl |= FIELD_PREP(MT_EFUSE_CTRL_MODE, MT_EE_READ);
	xone_mt76_write_register(mt, MT_EFUSE_CTRL, ctrl);

	if (!xone_mt76_poll(mt, MT_EFUSE_CTRL, MT_EFUSE_CTRL_KICK, 0))
		return -ETIMEDOUT;

	for (i = 0; i < len; i += sizeof(u32)) {
		/* block data offset (multiple of 32 bits) */
		offset = (addr & GENMASK(3, 2)) + i;
		val = xone_mt76_read_register(mt, MT_EFUSE_DATA_BASE + offset);
		remaining = min_t(int, len - i, sizeof(u32));

		memcpy(data + i, &val, remaining);
	}

	return 0;
}

struct sk_buff *xone_mt76_alloc_message(int len, gfp_t gfp)
{
	struct sk_buff *skb;

	/* up to 4 bytes of padding */
	skb = alloc_skb(MT_CMD_HDR_LEN + len + sizeof(u32) + MT_CMD_HDR_LEN,
			gfp);
	if (!skb)
		return NULL;

	skb_reserve(skb, MT_CMD_HDR_LEN);

	return skb;
}

static void xone_mt76_prep_message(struct sk_buff *skb, u32 info)
{
	int len, pad;

	/* padding and trailer */
	len = round_up(skb->len, sizeof(u32));
	pad = len - skb->len + MT_CMD_HDR_LEN;

	put_unaligned_le32(info | FIELD_PREP(MT_MCU_MSG_LEN, len),
			   skb_push(skb, MT_CMD_HDR_LEN));
	memset(skb_put(skb, pad), 0, pad);
}

void xone_mt76_prep_command(struct sk_buff *skb, enum mt76_mcu_cmd cmd)
{
	xone_mt76_prep_message(skb, MT_MCU_MSG_TYPE_CMD |
			       FIELD_PREP(MT_MCU_MSG_PORT, MT_CPU_TX_PORT) |
			       FIELD_PREP(MT_MCU_MSG_CMD_TYPE, cmd));
}

static int xone_mt76_send_command(struct xone_mt76 *mt, struct sk_buff *skb,
				  enum mt76_mcu_cmd cmd)
{
	int err;

	xone_mt76_prep_command(skb, cmd);

	err = usb_bulk_msg(mt->udev, usb_sndbulkpipe(mt->udev, XONE_MT_EP_OUT),
			   skb->data, skb->len, NULL, XONE_MT_USB_TIMEOUT);
	consume_skb(skb);

	return err;
}

static int xone_mt76_send_wlan(struct xone_mt76 *mt, struct sk_buff *skb)
{
	struct mt76_txwi txwi = {};
	int err;

	/* wait for acknowledgment */
	/* ignore wireless client identifier (WCID) */
	txwi.flags = cpu_to_le16(FIELD_PREP(MT_TXWI_FLAGS_MPDU_DENSITY,
					    IEEE80211_HT_MPDU_DENSITY_4));
	txwi.rate = cpu_to_le16(FIELD_PREP(MT_RXWI_RATE_PHY, MT_PHY_TYPE_OFDM));
	txwi.ack_ctl = MT_TXWI_ACK_CTL_REQ;
	txwi.wcid = 0xff;
	txwi.len_ctl = cpu_to_le16(skb->len);

	memcpy(skb_push(skb, sizeof(txwi)), &txwi, sizeof(txwi));

	/* enhanced distributed channel access (EDCA) */
	/* wireless information valid (WIV) */
	xone_mt76_prep_message(skb,
			       FIELD_PREP(MT_TXD_INFO_DPORT, MT_WLAN_PORT) |
			       FIELD_PREP(MT_TXD_INFO_QSEL, MT_QSEL_EDCA) |
			       MT_TXD_INFO_WIV |
			       MT_TXD_INFO_80211);

	err = usb_bulk_msg(mt->udev, usb_sndbulkpipe(mt->udev, XONE_MT_EP_OUT),
			   skb->data, skb->len, NULL, XONE_MT_USB_TIMEOUT);
	consume_skb(skb);

	return err;
}

static int xone_mt76_select_function(struct xone_mt76 *mt,
				     enum mt76_mcu_function func, u32 val)
{
	struct sk_buff *skb;

	skb = xone_mt76_alloc_message(sizeof(u32) * 2, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	put_unaligned_le32(func, skb_put(skb, sizeof(u32)));
	put_unaligned_le32(val, skb_put(skb, sizeof(u32)));

	return xone_mt76_send_command(mt, skb, MT_CMD_FUN_SET_OP);
}

static int xone_mt76_load_cr(struct xone_mt76 *mt, enum mt76_mcu_cr_mode mode)
{
	struct sk_buff *skb;
	struct xone_mt76_msg_load_cr msg = {};

	skb = xone_mt76_alloc_message(sizeof(msg), GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	msg.mode = mode;
	skb_put_data(skb, &msg, sizeof(msg));

	return xone_mt76_send_command(mt, skb, MT_CMD_LOAD_CR);
}

static int xone_mt76_send_ms_command(struct xone_mt76 *mt,
				     enum xone_mt76_ms_command cmd,
				     void *data, int len)
{
	struct sk_buff *skb;

	skb = xone_mt76_alloc_message(sizeof(u32) + len, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	put_unaligned_le32(cmd, skb_put(skb, sizeof(u32)));
	skb_put_data(skb, data, len);

	/* send command to Microsoft's proprietary firmware */
	return xone_mt76_send_command(mt, skb, MT_CMD_INIT_GAIN_OP);
}

static int xone_mt76_write_burst(struct xone_mt76 *mt, u32 idx,
				 void *data, int len)
{
	struct sk_buff *skb;

	skb = xone_mt76_alloc_message(sizeof(idx) + len, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	/* register offset in memory */
	put_unaligned_le32(idx + MT_MCU_MEMMAP_WLAN, skb_put(skb, sizeof(idx)));
	skb_put_data(skb, data, len);

	return xone_mt76_send_command(mt, skb, MT_CMD_BURST_WRITE);
}

int xone_mt76_set_led_mode(struct xone_mt76 *mt, enum xone_mt76_led_mode mode)
{
	struct sk_buff *skb;

	skb = xone_mt76_alloc_message(sizeof(u32), GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	put_unaligned_le32(mode, skb_put(skb, sizeof(u32)));

	return xone_mt76_send_command(mt, skb, MT_CMD_LED_MODE_OP);
}

static int xone_mt76_set_power_mode(struct xone_mt76 *mt,
				    enum mt76_mcu_power_mode mode)
{
	struct sk_buff *skb;

	skb = xone_mt76_alloc_message(sizeof(u32), GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	put_unaligned_le32(mode, skb_put(skb, sizeof(u32)));

	return xone_mt76_send_command(mt, skb, MT_CMD_POWER_SAVING_OP);
}

static int xone_mt76_set_wow_enable(struct xone_mt76 *mt, bool enable)
{
	struct sk_buff *skb;

	skb = xone_mt76_alloc_message(sizeof(u32) + sizeof(u8) * 2, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	put_unaligned_le32(XONE_MT_WOW_ENABLE, skb_put(skb, sizeof(u32)));
	skb_put_u8(skb, enable);
	skb_put_u8(skb, mt->channel->index);

	return xone_mt76_send_command(mt, skb, MT_CMD_WOW_FEATURE);
}

static int xone_mt76_set_wow_traffic(struct xone_mt76 *mt,
				     enum xone_mt76_wow_traffic traffic)
{
	struct sk_buff *skb;

	skb = xone_mt76_alloc_message(sizeof(u32) + sizeof(u8), GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	put_unaligned_le32(XONE_MT_WOW_TRAFFIC, skb_put(skb, sizeof(u32)));
	skb_put_u8(skb, traffic);

	return xone_mt76_send_command(mt, skb, MT_CMD_WOW_FEATURE);
}

static int xone_mt76_switch_channel(struct xone_mt76 *mt,
				    struct xone_mt76_channel *chan)
{
	struct sk_buff *skb;
	struct xone_mt76_msg_switch_channel msg = {};

	skb = xone_mt76_alloc_message(sizeof(msg), GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	/* select TX and RX stream 1 */
	/* enable or disable scanning (unknown purpose) */
	msg.channel = chan->index;
	msg.tx_rx_setting = cpu_to_le16(0x0101);
	msg.bandwidth = chan->bandwidth;
	msg.tx_power = chan->power;
	msg.scan = chan->scan;
	skb_put_data(skb, &msg, sizeof(msg));

	return xone_mt76_send_command(mt, skb, MT_CMD_SWITCH_CHANNEL_OP);
}

static int xone_mt76_calibrate(struct xone_mt76 *mt,
			       enum mt76_mcu_calibration calib, u32 val)
{
	struct sk_buff *skb;

	skb = xone_mt76_alloc_message(sizeof(u32) * 2, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	put_unaligned_le32(calib, skb_put(skb, sizeof(u32)));
	put_unaligned_le32(val, skb_put(skb, sizeof(u32)));

	return xone_mt76_send_command(mt, skb, MT_CMD_CALIBRATION_OP);
}

static int xone_mt76_send_firmware_part(struct xone_mt76 *mt, u32 offset,
					const u8 *data, u32 len)
{
	struct sk_buff *skb;
	u32 pos, chunk_len, complete;
	int err;

	for (pos = 0; pos < len; pos += XONE_MT_FW_CHUNK_SIZE) {
		chunk_len = min_t(u32, len - pos, XONE_MT_FW_CHUNK_SIZE);

		skb = xone_mt76_alloc_message(chunk_len, GFP_KERNEL);
		if (!skb)
			return -ENOMEM;

		skb_put_data(skb, data + pos, chunk_len);
		chunk_len = roundup(chunk_len, sizeof(u32));

		xone_mt76_write_register(mt, MT_FCE_DMA_ADDR | MT_VEND_TYPE_CFG,
					 offset + pos);
		xone_mt76_write_register(mt, MT_FCE_DMA_LEN | MT_VEND_TYPE_CFG,
					 chunk_len << 16);

		err = xone_mt76_send_command(mt, skb, 0);
		if (err)
			return err;

		complete = 0xc0000000 | (chunk_len << 16);
		if (!xone_mt76_poll(mt, MT_FCE_DMA_LEN | MT_VEND_TYPE_CFG,
				    0xffffffff, complete))
			return -ETIMEDOUT;
	}

	return 0;
}

static int xone_mt76_send_firmware(struct xone_mt76 *mt,
				   const struct firmware *fw)
{
	const struct mt76_fw_header *hdr;
	u32 ilm_len, dlm_len;
	int err;

	if (fw->size < sizeof(*hdr))
		return -EINVAL;

	hdr = (const struct mt76_fw_header *)fw->data;
	ilm_len = le32_to_cpu(hdr->ilm_len);
	dlm_len = le32_to_cpu(hdr->dlm_len);

	if (fw->size != sizeof(*hdr) + ilm_len + dlm_len)
		return -EINVAL;

	dev_dbg(mt->dev, "%s: build=%.16s\n", __func__, hdr->build_time);

	/* configure DMA, enable FCE and packet DMA */
	xone_mt76_write_register(mt, MT_USB_U3DMA_CFG | MT_VEND_TYPE_CFG,
				 MT_USB_DMA_CFG_TX_BULK_EN |
				 MT_USB_DMA_CFG_RX_BULK_EN);
	xone_mt76_write_register(mt, MT_FCE_PSE_CTRL, 0x01);
	xone_mt76_write_register(mt, MT_TX_CPU_FROM_FCE_BASE_PTR, 0x00400230);
	xone_mt76_write_register(mt, MT_TX_CPU_FROM_FCE_MAX_COUNT, 0x01);
	xone_mt76_write_register(mt, MT_TX_CPU_FROM_FCE_CPU_DESC_IDX, 0x01);
	xone_mt76_write_register(mt, MT_FCE_PDMA_GLOBAL_CONF, 0x44);
	xone_mt76_write_register(mt, MT_FCE_SKIP_FS, 0x03);

	/* send instruction local memory */
	err = xone_mt76_send_firmware_part(mt, XONE_MT_FW_ILM_OFFSET,
					   fw->data + sizeof(*hdr), ilm_len);
	if (err)
		return err;

	/* send data local memory */
	return xone_mt76_send_firmware_part(mt, XONE_MT_FW_DLM_OFFSET,
					    fw->data + sizeof(*hdr) + ilm_len,
					    dlm_len);
}

static int xone_mt76_reset_firmware(struct xone_mt76 *mt)
{
	u32 val;
	int err;

	/* apply power-on RF patch */
	val = xone_mt76_read_register(mt, XONE_MT_RF_PATCH | MT_VEND_TYPE_CFG);
	xone_mt76_write_register(mt, XONE_MT_RF_PATCH | MT_VEND_TYPE_CFG,
				 val & ~BIT(19));

	err = xone_mt76_load_ivb(mt);
	if (err)
		return err;

	/* wait for reset */
	if (!xone_mt76_poll(mt, MT_FCE_DMA_ADDR | MT_VEND_TYPE_CFG,
			    0x80000000, 0x80000000))
		return -ETIMEDOUT;

	return 0;
}

int xone_mt76_load_firmware(struct xone_mt76 *mt, const char *name)
{
	const struct firmware *fw;
	int err;

	if (xone_mt76_read_register(mt, MT_FCE_DMA_ADDR | MT_VEND_TYPE_CFG)) {
		dev_dbg(mt->dev, "%s: resetting firmware...\n", __func__);
		return xone_mt76_reset_firmware(mt);
	}

	err = request_firmware(&fw, name, mt->dev);
	if (err) {
		if (err == -ENOENT)
			dev_err(mt->dev, "%s: firmware not found\n", __func__);

		return err;
	}

	err = xone_mt76_send_firmware(mt, fw);
	if (err)
		goto err_free_firmware;

	xone_mt76_write_register(mt, MT_FCE_DMA_ADDR | MT_VEND_TYPE_CFG, 0);

	err = xone_mt76_load_ivb(mt);
	if (err)
		goto err_free_firmware;

	if (!xone_mt76_poll(mt, MT_FCE_DMA_ADDR | MT_VEND_TYPE_CFG, 0x01, 0x01))
		err = -ETIMEDOUT;

err_free_firmware:
	release_firmware(fw);

	return err;
}

static const struct xone_mt76_channel
xone_mt76_channels[XONE_MT_NUM_CHANNELS] = {
	{ 0x01, XONE_MT_CH_2G_LOW, MT_PHY_BW_20, 0, true, 0 },
	{ 0x06, XONE_MT_CH_2G_MID, MT_PHY_BW_20, 0, true, 0 },
	{ 0x0b, XONE_MT_CH_2G_HIGH, MT_PHY_BW_20, 0, true, 0 },
	{ 0x24, XONE_MT_CH_5G_LOW, MT_PHY_BW_40, MT_CH_5G_UNII_1, true, 0 },
	{ 0x28, XONE_MT_CH_5G_LOW, MT_PHY_BW_40, MT_CH_5G_UNII_1, false, 0 },
	{ 0x2c, XONE_MT_CH_5G_HIGH, MT_PHY_BW_40, MT_CH_5G_UNII_1, true, 0 },
	{ 0x30, XONE_MT_CH_5G_HIGH, MT_PHY_BW_40, MT_CH_5G_UNII_1, false, 0 },
	{ 0x95, XONE_MT_CH_5G_LOW, MT_PHY_BW_80, MT_CH_5G_UNII_3, true, 0 },
	{ 0x99, XONE_MT_CH_5G_LOW, MT_PHY_BW_80, MT_CH_5G_UNII_3, false, 0 },
	{ 0x9d, XONE_MT_CH_5G_HIGH, MT_PHY_BW_80, MT_CH_5G_UNII_3, true, 0 },
	{ 0xa1, XONE_MT_CH_5G_HIGH, MT_PHY_BW_80, MT_CH_5G_UNII_3, false, 0 },
	{ 0xa5, XONE_MT_CH_5G_HIGH, MT_PHY_BW_80, MT_CH_5G_UNII_3, false, 0 },
};

static int xone_mt76_set_channel_candidates(struct xone_mt76 *mt)
{
	struct sk_buff *skb;
	u8 best_chan = mt->channel->index;
	u8 chan;
	int i, err;

	skb = alloc_skb(sizeof(u32) * 2 + sizeof(u32) * XONE_MT_NUM_CHANNELS,
			GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	put_unaligned_le32(1, skb_put(skb, sizeof(u32)));
	put_unaligned_le32(best_chan, skb_put(skb, sizeof(u32)));
	put_unaligned_le32(XONE_MT_NUM_CHANNELS - 1, skb_put(skb, sizeof(u32)));

	for (i = 0; i < XONE_MT_NUM_CHANNELS; i++) {
		chan = mt->channels[i].index;
		if (chan != best_chan)
			put_unaligned_le32(chan, skb_put(skb, sizeof(u32)));
	}

	err = xone_mt76_send_ms_command(mt, XONE_MT_SET_CHAN_CANDIDATES,
					skb->data, skb->len);
	consume_skb(skb);

	return err;
}

static int xone_mt76_get_channel_power(struct xone_mt76 *mt,
				       struct xone_mt76_channel *chan)
{
	u16 addr;
	u8 idx, target, offset;
	u8 entry[8];
	int err;

	if (chan->bandwidth == MT_PHY_BW_20) {
		addr = MT_EE_TX_POWER_0_START_2G;
		idx = 4;
	} else {
		/* each group has its own power table */
		addr = MT_EE_TX_POWER_0_START_5G +
		       chan->group * MT_TX_POWER_GROUP_SIZE_5G;
		idx = 5;
	}

	err = xone_mt76_read_efuse(mt, addr, entry, sizeof(entry));
	if (err) {
		dev_err(mt->dev, "%s: read EFUSE failed: %d\n", __func__, err);
		return err;
	}

	target = entry[idx];
	offset = entry[idx + chan->band];

	/* increase or decrease power by offset (in 0.5 dB steps) */
	if (offset & BIT(7))
		chan->power = (offset & BIT(6)) ?
			      target + (offset & GENMASK(5, 0)) :
			      target - (offset & GENMASK(5, 0));
	else
		chan->power = target;

	return 0;
}

static int xone_mt76_evaluate_channels(struct xone_mt76 *mt)
{
	struct xone_mt76_channel *chan;
	int i, err, pow = 0;

	mt->channel = NULL;

	memcpy(mt->channels, xone_mt76_channels, sizeof(xone_mt76_channels));

	for (i = 0; i < XONE_MT_NUM_CHANNELS; i++) {
		chan = &mt->channels[i];

		/* original driver increases power for channels 0x24 to 0x30 */
		err = xone_mt76_get_channel_power(mt, chan);
		if (err)
			return err;

		err = xone_mt76_switch_channel(mt, chan);
		if (err)
			return err;
		
		/* pick the highest power channel seen first */
		/* the last channel might not be the best one */
		if (chan->power > pow) {
			mt->channel = chan;
			pow = chan->power;
		}

		dev_dbg(mt->dev, "%s: channel=%u, power=%u\n", __func__,
			chan->index, chan->power);
	}

	if (mt->channel == NULL)
		mt->channel = chan;

	return 0;
}

static int xone_mt76_init_channels(struct xone_mt76 *mt)
{
	int err;

	/* enable promiscuous mode */
	xone_mt76_write_register(mt, MT_RX_FILTR_CFG, 0x014f13);

	err = xone_mt76_evaluate_channels(mt);
	if (err)
		return err;

	/* disable promiscuous mode */
	xone_mt76_write_register(mt, MT_RX_FILTR_CFG, 0x017f17);

	dev_dbg(mt->dev, "%s: channel=%u\n", __func__, mt->channel->index);

	mt->channel->scan = true;

	err = xone_mt76_switch_channel(mt, mt->channel);
	if (err)
		return err;

	err = xone_mt76_set_power_mode(mt, MT_RADIO_OFF);
	if (err)
		return err;

	msleep(50);

	err = xone_mt76_set_power_mode(mt, MT_RADIO_ON);
	if (err)
		return err;

	mt->channel->scan = false;

	err = xone_mt76_switch_channel(mt, mt->channel);
	if (err)
		return err;

	return xone_mt76_set_channel_candidates(mt);
}

static int xone_mt76_set_idle_time(struct xone_mt76 *mt)
{
	__le32 time = cpu_to_le32(64);

	/* prevent wireless clients from disconnecting when idle */
	return xone_mt76_send_ms_command(mt, XONE_MT_SET_IDLE_TIME,
					 &time, sizeof(time));
}

static int xone_mt76_init_address(struct xone_mt76 *mt)
{
	int err;

	err = xone_mt76_read_efuse(mt, MT_EE_MAC_ADDR,
				   mt->address, sizeof(mt->address));
	if (err)
		return err;

	dev_dbg(mt->dev, "%s: address=%pM\n", __func__, mt->address);

	/* some addresses start with 6c:5d:3a */
	/* clients only connect to 62:45:bx:xx:xx:xx */
	if (mt->address[0] != 0x62) {
		mt->address[0] = 0x62;
		mt->address[1] = 0x45;
		mt->address[2] = 0xbd;
	}

	err = xone_mt76_write_burst(mt, MT_MAC_ADDR_DW0,
				    mt->address, sizeof(mt->address));
	if (err)
		return err;

	err = xone_mt76_write_burst(mt, MT_MAC_BSSID_DW0,
				    mt->address, sizeof(mt->address));
	if (err)
		return err;

	return xone_mt76_send_ms_command(mt, XONE_MT_SET_MAC_ADDRESS,
					 mt->address, sizeof(mt->address));
}

static int xone_mt76_calibrate_crystal(struct xone_mt76 *mt)
{
	u8 trim[4];
	u16 val;
	s8 offset;
	u32 ctrl;
	int err;

	err = xone_mt76_read_efuse(mt, MT_EE_XTAL_TRIM_2, trim, sizeof(trim));
	if (err)
		return err;

	val = (trim[3] << 8) | trim[2];
	offset = val & GENMASK(6, 0);
	if ((val & 0xff) == 0xff)
		offset = 0;
	else if (val & BIT(7))
		offset = -offset;

	val >>= 8;
	if (!val || val == 0xff) {
		err = xone_mt76_read_efuse(mt, MT_EE_XTAL_TRIM_1, trim,
					   sizeof(trim));
		if (err)
			return err;

		val = (trim[3] << 8) | trim[2];
		val &= 0xff;
		if (!val || val == 0xff)
			val = 0x14;
	}

	val = (val & GENMASK(6, 0)) + offset;
	ctrl = xone_mt76_read_register(mt, MT_XO_CTRL5 | MT_VEND_TYPE_CFG);
	xone_mt76_write_register(mt, MT_XO_CTRL5 | MT_VEND_TYPE_CFG,
				 (ctrl & ~MT_XO_CTRL5_C2_VAL) | (val << 8));
	xone_mt76_write_register(mt, MT_XO_CTRL6 | MT_VEND_TYPE_CFG,
				 MT_XO_CTRL6_C2_CTRL);
	xone_mt76_write_register(mt, MT_CMB_CTRL, 0x0091a7ff);

	return 0;
}

static int xone_mt76_calibrate_radio(struct xone_mt76 *mt)
{
	int err;

	/* configure automatic gain control (AGC) */
	xone_mt76_write_register(mt, MT_BBP(AGC, 8), 0x18365efa);
	xone_mt76_write_register(mt, MT_BBP(AGC, 9), 0x18365efa);

	/* reset required for reliable WLAN associations */
	xone_mt76_write_register(mt, MT_MAC_SYS_CTRL, 0);
	xone_mt76_write_register(mt, MT_RF_BYPASS_0, 0);
	xone_mt76_write_register(mt, MT_RF_SETTING_0, 0);

	err = xone_mt76_calibrate(mt, MT_MCU_CAL_TEMP_SENSOR, 0);
	if (err)
		return err;

	err = xone_mt76_calibrate(mt, MT_MCU_CAL_RXDCOC, 1);
	if (err)
		return err;

	err = xone_mt76_calibrate(mt, MT_MCU_CAL_RC, 0);
	if (err)
		return err;

	xone_mt76_write_register(mt, MT_MAC_SYS_CTRL,
				 MT_MAC_SYS_CTRL_ENABLE_RX |
				 MT_MAC_SYS_CTRL_ENABLE_TX);

	return 0;
}

static void xone_mt76_init_registers(struct xone_mt76 *mt)
{
	xone_mt76_write_register(mt, MT_MAC_SYS_CTRL,
				 MT_MAC_SYS_CTRL_RESET_BBP |
				 MT_MAC_SYS_CTRL_RESET_CSR);
	xone_mt76_write_register(mt, MT_USB_DMA_CFG, 0);
	xone_mt76_write_register(mt, MT_MAC_SYS_CTRL, 0);
	xone_mt76_write_register(mt, MT_PWR_PIN_CFG, 0);
	xone_mt76_write_register(mt, MT_LDO_CTRL_1, 0x6b006464);
	xone_mt76_write_register(mt, MT_WPDMA_GLO_CFG, 0x70);
	xone_mt76_write_register(mt, MT_WMM_AIFSN, 0x2273);
	xone_mt76_write_register(mt, MT_WMM_CWMIN, 0x2344);
	xone_mt76_write_register(mt, MT_WMM_CWMAX, 0x34aa);
	xone_mt76_write_register(mt, MT_FCE_DMA_ADDR, 0x041200);
	xone_mt76_write_register(mt, MT_TSO_CTRL, 0);
	xone_mt76_write_register(mt, MT_PBF_SYS_CTRL, 0x080c00);
	xone_mt76_write_register(mt, MT_PBF_TX_MAX_PCNT, 0x1fbf1f1f);
	xone_mt76_write_register(mt, MT_FCE_PSE_CTRL, 0x01);
	xone_mt76_write_register(mt, MT_MAC_SYS_CTRL,
				 MT_MAC_SYS_CTRL_ENABLE_RX |
				 MT_MAC_SYS_CTRL_ENABLE_TX);
	xone_mt76_write_register(mt, MT_AUTO_RSP_CFG, 0x13);
	xone_mt76_write_register(mt, MT_MAX_LEN_CFG, 0x3e3fff);
	xone_mt76_write_register(mt, MT_AMPDU_MAX_LEN_20M1S, 0xfffc9855);
	xone_mt76_write_register(mt, MT_AMPDU_MAX_LEN_20M2S, 0xff);
	xone_mt76_write_register(mt, MT_BKOFF_SLOT_CFG, 0x0109);
	xone_mt76_write_register(mt, MT_PWR_PIN_CFG, 0);
	xone_mt76_write_register(mt, MT_EDCA_CFG_AC(0), 0x064320);
	xone_mt76_write_register(mt, MT_EDCA_CFG_AC(1), 0x0a4700);
	xone_mt76_write_register(mt, MT_EDCA_CFG_AC(2), 0x043238);
	xone_mt76_write_register(mt, MT_EDCA_CFG_AC(3), 0x03212f);
	xone_mt76_write_register(mt, MT_TX_PIN_CFG, 0x150f0f);
	xone_mt76_write_register(mt, MT_TX_SW_CFG0, 0x101001);
	xone_mt76_write_register(mt, MT_TX_SW_CFG1, 0x010000);
	xone_mt76_write_register(mt, MT_TXOP_CTRL_CFG, 0x10583f);
	xone_mt76_write_register(mt, MT_TX_TIMEOUT_CFG, 0x0a0f90);
	xone_mt76_write_register(mt, MT_TX_RETRY_CFG, 0x47d01f0f);
	xone_mt76_write_register(mt, MT_CCK_PROT_CFG, 0x03f40003);
	xone_mt76_write_register(mt, MT_OFDM_PROT_CFG, 0x03f40003);
	xone_mt76_write_register(mt, MT_MM20_PROT_CFG, 0x01742004);
	xone_mt76_write_register(mt, MT_GF20_PROT_CFG, 0x01742004);
	xone_mt76_write_register(mt, MT_GF40_PROT_CFG, 0x03f42084);
	xone_mt76_write_register(mt, MT_EXP_ACK_TIME, 0x2c00dc);
	xone_mt76_write_register(mt, MT_TX_ALC_CFG_2, 0x22160a00);
	xone_mt76_write_register(mt, MT_TX_ALC_CFG_3, 0x22160a76);
	xone_mt76_write_register(mt, MT_TX_ALC_CFG_0, 0x3f3f1818);
	xone_mt76_write_register(mt, MT_TX_ALC_CFG_4, 0x0606);
	xone_mt76_write_register(mt, MT_PIFS_TX_CFG, 0x060fff);
	xone_mt76_write_register(mt, MT_RX_FILTR_CFG, 0x017f17);
	xone_mt76_write_register(mt, MT_LEGACY_BASIC_RATE, 0x017f);
	xone_mt76_write_register(mt, MT_HT_BASIC_RATE, 0x8003);
	xone_mt76_write_register(mt, MT_PN_PAD_MODE, 0x02);
	xone_mt76_write_register(mt, MT_TXOP_HLDR_ET, 0x02);
	xone_mt76_write_register(mt, MT_TX_PROT_CFG6, 0xe3f42004);
	xone_mt76_write_register(mt, MT_TX_PROT_CFG7, 0xe3f42084);
	xone_mt76_write_register(mt, MT_TX_PROT_CFG8, 0xe3f42104);
	xone_mt76_write_register(mt, MT_DACCLK_EN_DLY_CFG, 0);
	xone_mt76_write_register(mt, MT_RF_PA_MODE_ADJ0, 0xee000000);
	xone_mt76_write_register(mt, MT_RF_PA_MODE_ADJ1, 0xee000000);
	xone_mt76_write_register(mt, MT_TX0_RF_GAIN_CORR, 0x0f3c3c3c);
	xone_mt76_write_register(mt, MT_TX1_RF_GAIN_CORR, 0x0f3c3c3c);
	xone_mt76_write_register(mt, MT_PBF_CFG, 0x1efebcf5);
	xone_mt76_write_register(mt, MT_PAUSE_ENABLE_CONTROL1, 0x0a);
	xone_mt76_write_register(mt, MT_RF_BYPASS_0, 0x7f000000);
	xone_mt76_write_register(mt, MT_RF_SETTING_0, 0x1a800000);
	xone_mt76_write_register(mt, MT_XIFS_TIME_CFG, 0x33a40e0a);
	xone_mt76_write_register(mt, MT_FCE_L2_STUFF, 0x03ff0223);
	xone_mt76_write_register(mt, MT_TX_RTS_CFG, 0);
	xone_mt76_write_register(mt, MT_BEACON_TIME_CFG, 0x0640);
	xone_mt76_write_register(mt, MT_EXT_CCA_CFG, 0xf0e4);
	xone_mt76_write_register(mt, MT_CH_TIME_CFG, 0x015f);
}

static u16 xone_mt76_get_chip_id(struct xone_mt76 *mt)
{
	u8 id[4];

	if (xone_mt76_read_efuse(mt, MT_EE_CHIP_ID, &id, sizeof(id)))
		return 0;

	return (id[1] << 8) | id[2];
}

int xone_mt76_init_radio(struct xone_mt76 *mt)
{
	int err;

	dev_dbg(mt->dev, "%s: id=0x%04x\n", __func__,
		xone_mt76_get_chip_id(mt));

	err = xone_mt76_select_function(mt, MT_Q_SELECT, 1);
	if (err)
		return err;

	err = xone_mt76_set_power_mode(mt, MT_RADIO_ON);
	if (err)
		return err;

	err = xone_mt76_load_cr(mt, MT_RF_BBP_CR);
	if (err)
		return err;

	xone_mt76_init_registers(mt);

	err = xone_mt76_calibrate_crystal(mt);
	if (err)
		return err;

	err = xone_mt76_init_address(mt);
	if (err)
		return err;

	err = xone_mt76_set_idle_time(mt);
	if (err)
		return err;

	err = xone_mt76_calibrate_radio(mt);
	if (err)
		return err;

	err = xone_mt76_init_channels(mt);
	if (err)
		return err;

	/* mandatory delay after channel change */
	msleep(1000);

	return xone_mt76_set_pairing(mt, false);
}

int xone_mt76_suspend_radio(struct xone_mt76 *mt)
{
	int err;

	xone_mt76_write_register(mt, MT_MAC_SYS_CTRL, 0);

	/* enable wake-on-wireless */
	err = xone_mt76_set_wow_enable(mt, true);
	if (err)
		return err;

	err = xone_mt76_set_wow_traffic(mt, XONE_MT_WOW_TO_HOST);
	if (err)
		return err;

	dev_dbg(mt->dev, "%s: suspended\n", __func__);

	return 0;
}

int xone_mt76_resume_radio(struct xone_mt76 *mt)
{
	int err;

	err = xone_mt76_set_wow_traffic(mt, XONE_MT_WOW_TO_FIRMWARE);
	if (err)
		return err;

	/* disable wake-on-wireless */
	err = xone_mt76_set_wow_enable(mt, false);
	if (err)
		return err;

	err = xone_mt76_switch_channel(mt, mt->channel);
	if (err)
		return err;

	err = xone_mt76_set_pairing(mt, false);
	if (err)
		return err;

	xone_mt76_write_register(mt, MT_MAC_SYS_CTRL,
				 MT_MAC_SYS_CTRL_ENABLE_RX |
				 MT_MAC_SYS_CTRL_ENABLE_TX);

	dev_dbg(mt->dev, "%s: resumed\n", __func__);

	return 0;
}

static int xone_mt76_write_beacon(struct xone_mt76 *mt, bool pair)
{
	struct sk_buff *skb;
	struct mt76_txwi txwi = {};
	struct ieee80211_mgmt mgmt = {};
	u8 data[] = {
		/* information element with Microsoft's OUI (00:50:f2) */
		/* probably includes the selected channel pair */
		0x00, 0x00, 0xdd, 0x10, 0x00, 0x50, 0xf2, 0x11,
		0x01, 0x10, pair, 0xa5, 0x30, 0x99, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
	};
	int mgmt_len = sizeof(struct ieee80211_hdr_3addr) +
		       sizeof(mgmt.u.beacon);
	int err;

	skb = alloc_skb(sizeof(txwi) + mgmt_len + sizeof(data), GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	/* generate beacon timestamp */
	/* use hardware sequence control */
	txwi.flags = cpu_to_le16(MT_TXWI_FLAGS_TS);
	txwi.rate = cpu_to_le16(FIELD_PREP(MT_RXWI_RATE_PHY, MT_PHY_TYPE_OFDM));
	txwi.ack_ctl = MT_TXWI_ACK_CTL_NSEQ;
	txwi.len_ctl = cpu_to_le16(mgmt_len + sizeof(data));

	mgmt.frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
					 IEEE80211_STYPE_BEACON);
	eth_broadcast_addr(mgmt.da);
	memcpy(mgmt.sa, mt->address, ETH_ALEN);
	memcpy(mgmt.bssid, mt->address, ETH_ALEN);

	/* default beacon interval (100 ms) */
	/* original capability info */
	mgmt.u.beacon.beacon_int = cpu_to_le16(100);
	mgmt.u.beacon.capab_info = cpu_to_le16(0xc631);

	skb_put_data(skb, &txwi, sizeof(txwi));
	skb_put_data(skb, &mgmt, mgmt_len);
	skb_put_data(skb, data, sizeof(data));

	err = xone_mt76_write_burst(mt, MT_BEACON_BASE, skb->data, skb->len);
	consume_skb(skb);

	return err;
}

int xone_mt76_set_pairing(struct xone_mt76 *mt, bool enable)
{
	int err;

	err = xone_mt76_write_beacon(mt, enable);
	if (err)
		return err;

	/* enable timing synchronization function (TSF) timer */
	/* enable target beacon transmission time (TBTT) timer */
	/* set TSF timer to AP mode */
	/* activate beacon transmission */
	xone_mt76_write_register(mt, MT_BEACON_TIME_CFG,
				 MT_BEACON_TIME_CFG_BEACON_TX |
				 MT_BEACON_TIME_CFG_TBTT_EN |
				 MT_BEACON_TIME_CFG_SYNC_MODE |
				 MT_BEACON_TIME_CFG_TIMER_EN |
				 FIELD_PREP(MT_BEACON_TIME_CFG_INTVAL, 0x0640));

	return 0;
}

int xone_mt76_pair_client(struct xone_mt76 *mt, u8 *addr)
{
	struct sk_buff *skb;
	struct ieee80211_hdr_3addr hdr = {};
	u8 data[] = { 0x00, 0x45, 0x55, 0x01, 0x0f, 0x8f, 0xff, 0x87, 0x1f };

	skb = xone_mt76_alloc_message(sizeof(struct mt76_txwi) + sizeof(hdr) +
				      sizeof(u8) * 2 + sizeof(data),
				      GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	hdr.frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
					XONE_MT_WLAN_RESERVED);
	memcpy(hdr.addr1, addr, ETH_ALEN);
	memcpy(hdr.addr2, mt->address, ETH_ALEN);
	memcpy(hdr.addr3, mt->address, ETH_ALEN);

	skb_reserve(skb, sizeof(struct mt76_txwi));
	skb_put_data(skb, &hdr, sizeof(hdr));
	skb_put_u8(skb, XONE_MT_WLAN_RESERVED);
	skb_put_u8(skb, XONE_MT_CLIENT_PAIR_RESP);
	skb_put_data(skb, data, sizeof(data));

	return xone_mt76_send_wlan(mt, skb);
}

int xone_mt76_associate_client(struct xone_mt76 *mt, u8 wcid, u8 *addr)
{
	struct sk_buff *skb;
	struct ieee80211_mgmt mgmt = {};
	u8 data[] = { wcid - 1, 0x00, 0x00, 0x00, 0x40, 0x1f, 0x00, 0x00 };
	int mgmt_len = sizeof(struct ieee80211_hdr_3addr) +
		       sizeof(mgmt.u.assoc_resp);
	int err;

	skb = xone_mt76_alloc_message(sizeof(struct mt76_txwi) + mgmt_len + 8,
				      GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	mgmt.frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
					 IEEE80211_STYPE_ASSOC_RESP);
	memcpy(mgmt.da, addr, ETH_ALEN);
	memcpy(mgmt.sa, mt->address, ETH_ALEN);
	memcpy(mgmt.bssid, mt->address, ETH_ALEN);

	/* original status code and association ID */
	mgmt.u.assoc_resp.status_code = cpu_to_le16(0x0110);
	mgmt.u.assoc_resp.aid = cpu_to_le16(0x0f00);

	skb_reserve(skb, sizeof(struct mt76_txwi));
	skb_put_data(skb, &mgmt, mgmt_len);
	memset(skb_put(skb, 8), 0, 8);

	err = xone_mt76_write_burst(mt, MT_WCID_ADDR(wcid), addr, ETH_ALEN);
	if (err)
		goto err_free_skb;

	err = xone_mt76_send_ms_command(mt, XONE_MT_ADD_CLIENT,
					data, sizeof(data));
	if (err)
		goto err_free_skb;

	return xone_mt76_send_wlan(mt, skb);

err_free_skb:
	kfree_skb(skb);

	return err;
}

int xone_mt76_send_client_command(struct xone_mt76 *mt, u8 wcid, u8 *addr,
				  enum xone_mt76_client_command cmd,
				  u8 *data, int len)
{
	struct sk_buff *skb;
	struct mt76_txwi txwi = {};
	struct ieee80211_hdr_3addr hdr = {};
	u8 info[] = {
		0x00, 0x00, 0x00, wcid - 1, 0x00, 0x00, 0x00, 0x00,
	};

	skb = xone_mt76_alloc_message(sizeof(info) + sizeof(txwi) +
				      sizeof(hdr) + sizeof(u8) * 2 + len,
				      GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	/* wait for acknowledgment */
	txwi.flags = cpu_to_le16(FIELD_PREP(MT_TXWI_FLAGS_MPDU_DENSITY,
					    IEEE80211_HT_MPDU_DENSITY_4));
	txwi.rate = cpu_to_le16(FIELD_PREP(MT_RXWI_RATE_PHY, MT_PHY_TYPE_OFDM));
	txwi.ack_ctl = MT_TXWI_ACK_CTL_REQ;
	txwi.wcid = wcid - 1;
	txwi.len_ctl = cpu_to_le16(sizeof(hdr) + sizeof(u8) * 2 + len);

	hdr.frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
					XONE_MT_WLAN_RESERVED);
	memcpy(hdr.addr1, addr, ETH_ALEN);
	memcpy(hdr.addr2, mt->address, ETH_ALEN);
	memcpy(hdr.addr3, mt->address, ETH_ALEN);

	skb_put_data(skb, info, sizeof(info));
	skb_put_data(skb, &txwi, sizeof(txwi));
	skb_put_data(skb, &hdr, sizeof(hdr));
	skb_put_u8(skb, XONE_MT_WLAN_RESERVED);
	skb_put_u8(skb, cmd);

	if (data)
		skb_put_data(skb, data, len);

	return xone_mt76_send_command(mt, skb, 0);
}

int xone_mt76_set_client_key(struct xone_mt76 *mt, u8 wcid, u8 *key, int len)
{
	u8 iv[] = { 0x01, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00 };
	__le32 attr = cpu_to_le32(FIELD_PREP(MT_WCID_ATTR_PKEY_MODE,
					     MT_CIPHER_AES_CCMP) |
				  MT_WCID_ATTR_PAIRWISE);
	int err;

	if (len != XONE_MT_WCID_KEY_LEN)
		return -EINVAL;

	err = xone_mt76_write_burst(mt, MT_WCID_KEY(wcid), key, len);
	if (err)
		return err;

	err = xone_mt76_write_burst(mt, MT_WCID_IV(wcid), iv, sizeof(iv));
	if (err)
		return err;

	return xone_mt76_write_burst(mt, MT_WCID_ATTR(wcid),
				     &attr, sizeof(attr));
}

int xone_mt76_remove_client(struct xone_mt76 *mt, u8 wcid)
{
	u8 data[] = { wcid - 1, 0x00, 0x00, 0x00 };
	u8 addr[ETH_ALEN] = {};
	u8 iv[8] = {};
	u32 attr = 0;
	u8 key[XONE_MT_WCID_KEY_LEN] = {};
	int err;

	err = xone_mt76_send_ms_command(mt, XONE_MT_REMOVE_CLIENT,
					data, sizeof(data));
	if (err)
		return err;

	err = xone_mt76_write_burst(mt, MT_WCID_ADDR(wcid), addr, sizeof(addr));
	if (err)
		return err;

	err = xone_mt76_write_burst(mt, MT_WCID_IV(wcid), iv, sizeof(iv));
	if (err)
		return err;

	err = xone_mt76_write_burst(mt, MT_WCID_ATTR(wcid),
				    &attr, sizeof(attr));
	if (err)
		return err;

	return xone_mt76_write_burst(mt, MT_WCID_KEY(wcid), key, sizeof(key));
}
