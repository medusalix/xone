// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2021 Severin von Wnuck-Lipinski <severinvonw@outlook.de>
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/bitfield.h>
#include <linux/version.h>
#include <linux/usb.h>
#include <linux/sysfs.h>
#include <linux/ieee80211.h>
#include <net/cfg80211.h>

#include "mt76.h"
#include "../bus/bus.h"

#define XONE_DONGLE_NUM_IN_URBS 12
#define XONE_DONGLE_NUM_OUT_URBS 12

#define XONE_DONGLE_LEN_CMD_PKT 0x0654
#define XONE_DONGLE_LEN_WLAN_PKT 0x8400

#define XONE_DONGLE_MAX_CLIENTS 16

/* autosuspend delay in ms */
#define XONE_DONGLE_SUSPEND_DELAY 60000

#define XONE_DONGLE_PAIRING_TIMEOUT msecs_to_jiffies(30000)
#define XONE_DONGLE_PWR_OFF_TIMEOUT msecs_to_jiffies(5000)

enum xone_dongle_queue {
	XONE_DONGLE_QUEUE_DATA = 0x00,
	XONE_DONGLE_QUEUE_AUDIO = 0x02,
};

struct xone_dongle_skb_cb {
	struct xone_dongle *dongle;
	struct urb *urb;
};

struct xone_dongle_client {
	struct xone_dongle *dongle;
	u8 wcid;
	u8 address[ETH_ALEN];
	bool encryption_enabled;

	struct gip_adapter *adapter;
};

struct xone_dongle_event {
	enum xone_dongle_event_type {
		XONE_DONGLE_EVT_ADD_CLIENT,
		XONE_DONGLE_EVT_REMOVE_CLIENT,
		XONE_DONGLE_EVT_PAIR_CLIENT,
		XONE_DONGLE_EVT_ENABLE_PAIRING,
		XONE_DONGLE_EVT_ENABLE_ENCRYPTION,
	} type;

	struct xone_dongle *dongle;
	u8 address[ETH_ALEN];
	u8 wcid;

	struct work_struct work;
};

struct xone_dongle {
	struct xone_mt76 mt;

	struct usb_anchor urbs_in_idle;
	struct usb_anchor urbs_in_busy;
	struct usb_anchor urbs_out_idle;
	struct usb_anchor urbs_out_busy;

	/* serializes pairing changes */
	struct mutex pairing_lock;
	struct delayed_work pairing_work;
	bool pairing;

	/* serializes access to clients array */
	spinlock_t clients_lock;
	struct xone_dongle_client *clients[XONE_DONGLE_MAX_CLIENTS];
	atomic_t client_count;
	wait_queue_head_t disconnect_wait;

	struct workqueue_struct *event_wq;
};

static void xone_dongle_prep_packet(struct xone_dongle_client *client,
				    struct sk_buff *skb,
				    enum xone_dongle_queue queue)
{
	struct ieee80211_qos_hdr hdr = {};
	struct mt76_txwi txwi = {};
	u8 data[] = {
		0x00, 0x00, queue, client->wcid - 1, 0x00, 0x00, 0x00, 0x00,
	};

	/* frame is sent from AP (DS) */
	/* duration is the time required to transmit (in μs) */
	hdr.frame_control = cpu_to_le16(IEEE80211_FTYPE_DATA |
					IEEE80211_STYPE_QOS_DATA |
					IEEE80211_FCTL_FROMDS);

	/* encrypt frame on transmission */
	if (client->encryption_enabled)
		hdr.frame_control |= cpu_to_le16(IEEE80211_FCTL_PROTECTED);

	hdr.duration_id = cpu_to_le16(144);
	memcpy(hdr.addr1, client->address, ETH_ALEN);
	memcpy(hdr.addr2, client->dongle->mt.address, ETH_ALEN);
	memcpy(hdr.addr3, client->dongle->mt.address, ETH_ALEN);

	/* wait for acknowledgment */
	txwi.flags = cpu_to_le16(FIELD_PREP(MT_TXWI_FLAGS_MPDU_DENSITY,
					    IEEE80211_HT_MPDU_DENSITY_4));
	txwi.rate = cpu_to_le16(FIELD_PREP(MT_RXWI_RATE_PHY, MT_PHY_TYPE_OFDM));
	txwi.ack_ctl = MT_TXWI_ACK_CTL_REQ;
	txwi.wcid = client->wcid - 1;
	txwi.len_ctl = cpu_to_le16(sizeof(hdr) + skb->len);

	memset(skb_push(skb, 2), 0, 2);
	memcpy(skb_push(skb, sizeof(hdr)), &hdr, sizeof(hdr));
	memcpy(skb_push(skb, sizeof(txwi)), &txwi, sizeof(txwi));
	memcpy(skb_push(skb, sizeof(data)), data, sizeof(data));

	xone_mt76_prep_command(skb, 0);
}

static int xone_dongle_get_buffer(struct gip_adapter *adap,
				  struct gip_adapter_buffer *buf)
{
	struct xone_dongle_client *client = dev_get_drvdata(&adap->dev);
	struct xone_dongle_skb_cb *cb;
	struct urb *urb;
	struct sk_buff *skb;

	urb = usb_get_from_anchor(&client->dongle->urbs_out_idle);
	if (!urb)
		return -ENOSPC;

	skb = xone_mt76_alloc_message(XONE_DONGLE_LEN_CMD_PKT, GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;

	/* command header + WCID data + TXWI + QoS header + padding */
	/* see xone_dongle_prep_packet and xone_mt76_prep_message */
	skb_reserve(skb, MT_CMD_HDR_LEN + 8 + sizeof(struct mt76_txwi) +
		    sizeof(struct ieee80211_qos_hdr) + 2 + MT_CMD_HDR_LEN);

	cb = (struct xone_dongle_skb_cb *)skb->cb;
	cb->dongle = client->dongle;
	cb->urb = urb;

	buf->context = skb;
	buf->data = skb->data;
	buf->length = skb_tailroom(skb);

	return 0;
}

static int xone_dongle_submit_buffer(struct gip_adapter *adap,
				     struct gip_adapter_buffer *buf)
{
	struct xone_dongle_client *client = dev_get_drvdata(&adap->dev);
	struct xone_dongle_skb_cb *cb;
	struct sk_buff *skb = buf->context;
	int err;

	skb_put(skb, buf->length);

	if (buf->type == GIP_BUF_DATA)
		xone_dongle_prep_packet(client, skb, XONE_DONGLE_QUEUE_DATA);
	else if (buf->type == GIP_BUF_AUDIO)
		xone_dongle_prep_packet(client, skb, XONE_DONGLE_QUEUE_AUDIO);
	else
		return -EINVAL;

	cb = (struct xone_dongle_skb_cb *)skb->cb;
	cb->urb->context = skb;
	cb->urb->transfer_buffer = skb->data;
	cb->urb->transfer_buffer_length = skb->len;
	usb_anchor_urb(cb->urb, &client->dongle->urbs_out_busy);

	err = usb_submit_urb(cb->urb, GFP_ATOMIC);
	if (err) {
		usb_unanchor_urb(cb->urb);
		usb_anchor_urb(cb->urb, &client->dongle->urbs_out_idle);
		dev_kfree_skb_any(skb);
	}

	usb_free_urb(cb->urb);

	return err;
}

static int xone_dongle_set_encryption_key(struct gip_adapter *adap,
					  u8 *key, int len)
{
	struct xone_dongle_client *client = dev_get_drvdata(&adap->dev);

	return xone_mt76_set_client_key(&client->dongle->mt, client->wcid,
					key, len);
}

static struct gip_adapter_ops xone_dongle_adapter_ops = {
	.get_buffer = xone_dongle_get_buffer,
	.submit_buffer = xone_dongle_submit_buffer,
	.set_encryption_key = xone_dongle_set_encryption_key,
};

static int xone_dongle_toggle_pairing(struct xone_dongle *dongle, bool enable)
{
	struct usb_interface *intf = to_usb_interface(dongle->mt.dev);
	enum xone_mt76_led_mode led;
	int err = 0;

	mutex_lock(&dongle->pairing_lock);

	/* pairing is already enabled/disabled */
	if (dongle->pairing == enable)
		goto err_unlock;

	err = xone_mt76_set_pairing(&dongle->mt, enable);
	if (err)
		goto err_unlock;

	if (enable)
		led = XONE_MT_LED_BLINK;
	else if (atomic_read(&dongle->client_count))
		led = XONE_MT_LED_ON;
	else
		led = XONE_MT_LED_OFF;

	err = xone_mt76_set_led_mode(&dongle->mt, led);
	if (err)
		goto err_unlock;

	if (enable)
		usb_autopm_get_interface(intf);
	else
		usb_autopm_put_interface(intf);

	dev_dbg(dongle->mt.dev, "%s: enabled=%d\n", __func__, enable);
	dongle->pairing = enable;

err_unlock:
	mutex_unlock(&dongle->pairing_lock);

	return err;
}

static void xone_dongle_pairing_timeout(struct work_struct *work)
{
	struct xone_dongle *dongle = container_of(to_delayed_work(work),
						  typeof(*dongle),
						  pairing_work);
	int err;

	err = xone_dongle_toggle_pairing(dongle, false);
	if (err)
		dev_err(dongle->mt.dev, "%s: disable pairing failed: %d\n",
			__func__, err);
}

static ssize_t xone_dongle_pairing_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct usb_interface *intf = to_usb_interface(dev);
	struct xone_dongle *dongle = usb_get_intfdata(intf);

	return sprintf(buf, "%d\n", dongle->pairing);
}

static ssize_t xone_dongle_pairing_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct usb_interface *intf = to_usb_interface(dev);
	struct xone_dongle *dongle = usb_get_intfdata(intf);
	bool enable;
	int err;

	err = kstrtobool(buf, &enable);
	if (err)
		return err;

	err = pm_runtime_resume_and_get(dev);
	if (err)
		return err;

	err = xone_dongle_toggle_pairing(dongle, enable);
	if (err)
		return err;

	pm_runtime_put(dev);

	return count;
}

static struct device_attribute xone_dongle_attr_pairing =
	__ATTR(pairing, 0644,
	       xone_dongle_pairing_show,
	       xone_dongle_pairing_store);

static struct attribute *xone_dongle_attrs[] = {
	&xone_dongle_attr_pairing.attr,
	NULL,
};
ATTRIBUTE_GROUPS(xone_dongle);

static struct xone_dongle_client *
xone_dongle_create_client(struct xone_dongle *dongle, u8 *addr)
{
	struct xone_dongle_client *client;
	int i, err;

	/* find free WCID */
	for (i = 0; i < XONE_DONGLE_MAX_CLIENTS; i++)
		if (!dongle->clients[i])
			break;

	if (i == XONE_DONGLE_MAX_CLIENTS)
		return ERR_PTR(-ENOSPC);

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return ERR_PTR(-ENOMEM);

	client->dongle = dongle;
	client->wcid = i + 1;
	memcpy(client->address, addr, ETH_ALEN);

	client->adapter = gip_create_adapter(dongle->mt.dev,
					     &xone_dongle_adapter_ops, 1);
	if (IS_ERR(client->adapter)) {
		err = PTR_ERR(client->adapter);
		kfree(client);
		return ERR_PTR(err);
	}

	dev_set_drvdata(&client->adapter->dev, client);

	return client;
}

static int xone_dongle_add_client(struct xone_dongle *dongle, u8 *addr)
{
	struct xone_dongle_client *client;
	int err;
	unsigned long flags;

	client = xone_dongle_create_client(dongle, addr);
	if (IS_ERR(client))
		return PTR_ERR(client);

	err = xone_mt76_associate_client(&dongle->mt, client->wcid, addr);
	if (err)
		goto err_free_client;

	if (!dongle->pairing) {
		err = xone_mt76_set_led_mode(&dongle->mt, XONE_MT_LED_ON);
		if (err)
			goto err_free_client;
	}

	dev_dbg(dongle->mt.dev, "%s: wcid=%d, address=%pM\n",
		__func__, client->wcid, addr);

	spin_lock_irqsave(&dongle->clients_lock, flags);
	dongle->clients[client->wcid - 1] = client;
	spin_unlock_irqrestore(&dongle->clients_lock, flags);

	atomic_inc(&dongle->client_count);
	usb_autopm_get_interface(to_usb_interface(dongle->mt.dev));

	return 0;

err_free_client:
	gip_destroy_adapter(client->adapter);
	kfree(client);

	return err;
}

static int xone_dongle_remove_client(struct xone_dongle *dongle, u8 wcid)
{
	struct xone_dongle_client *client;
	int err;
	unsigned long flags;

	client = dongle->clients[wcid - 1];
	if (!client)
		return 0;

	dev_dbg(dongle->mt.dev, "%s: wcid=%d, address=%pM\n",
		__func__, wcid, client->address);

	spin_lock_irqsave(&dongle->clients_lock, flags);
	dongle->clients[wcid - 1] = NULL;
	spin_unlock_irqrestore(&dongle->clients_lock, flags);

	gip_destroy_adapter(client->adapter);
	kfree(client);

	err = xone_mt76_remove_client(&dongle->mt, wcid);
	if (err)
		dev_err(dongle->mt.dev, "%s: remove failed: %d\n",
			__func__, err);

	/* turn off LED if all clients have disconnected */
	if (atomic_dec_and_test(&dongle->client_count) && !dongle->pairing)
		err = xone_mt76_set_led_mode(&dongle->mt, XONE_MT_LED_OFF);

	wake_up(&dongle->disconnect_wait);
	usb_autopm_put_interface(to_usb_interface(dongle->mt.dev));

	return err;
}

static int xone_dongle_pair_client(struct xone_dongle *dongle, u8 *addr)
{
	int err;

	dev_dbg(dongle->mt.dev, "%s: address=%pM\n", __func__, addr);

	err = xone_mt76_pair_client(&dongle->mt, addr);
	if (err)
		return err;

	return xone_dongle_toggle_pairing(dongle, false);
}

static int xone_dongle_enable_client_encryption(struct xone_dongle *dongle,
						u8 wcid)
{
	struct xone_dongle_client *client;
	u8 data[] = { 0x00, 0x00 };
	int err;

	client = dongle->clients[wcid - 1];
	if (!client)
		return -EINVAL;

	dev_dbg(dongle->mt.dev, "%s: wcid=%d, address=%pM\n",
		__func__, wcid, client->address);

	err = xone_mt76_send_client_command(&dongle->mt, wcid, client->address,
					    XONE_MT_CLIENT_ENABLE_ENCRYPTION,
					    data, sizeof(data));
	if (err)
		return err;

	client->encryption_enabled = true;

	return 0;
}

static void xone_dongle_handle_event(struct work_struct *work)
{
	struct xone_dongle_event *evt = container_of(work, typeof(*evt), work);
	int err = 0;

	switch (evt->type) {
	case XONE_DONGLE_EVT_ADD_CLIENT:
		err = xone_dongle_add_client(evt->dongle, evt->address);
		break;
	case XONE_DONGLE_EVT_REMOVE_CLIENT:
		err = xone_dongle_remove_client(evt->dongle, evt->wcid);
		break;
	case XONE_DONGLE_EVT_PAIR_CLIENT:
		err = xone_dongle_pair_client(evt->dongle, evt->address);
		break;
	case XONE_DONGLE_EVT_ENABLE_PAIRING:
		mod_delayed_work(system_wq, &evt->dongle->pairing_work,
				 XONE_DONGLE_PAIRING_TIMEOUT);
		err = xone_dongle_toggle_pairing(evt->dongle, true);
		break;
	case XONE_DONGLE_EVT_ENABLE_ENCRYPTION:
		err = xone_dongle_enable_client_encryption(evt->dongle,
							   evt->wcid);
		break;
	}

	if (err)
		dev_err(evt->dongle->mt.dev, "%s: handle event failed: %d\n",
			__func__, err);

	kfree(evt);
}

static struct xone_dongle_event *
xone_dongle_alloc_event(struct xone_dongle *dongle,
			enum xone_dongle_event_type type)
{
	struct xone_dongle_event *evt;

	evt = kzalloc(sizeof(*evt), GFP_ATOMIC);
	if (!evt)
		return NULL;

	evt->type = type;
	evt->dongle = dongle;
	INIT_WORK(&evt->work, xone_dongle_handle_event);

	return evt;
}

static int xone_dongle_handle_qos_data(struct xone_dongle *dongle,
				       struct sk_buff *skb, u8 wcid)
{
	struct xone_dongle_client *client;
	int err = 0;
	unsigned long flags;

	if (!wcid || wcid > XONE_DONGLE_MAX_CLIENTS)
		return 0;

	spin_lock_irqsave(&dongle->clients_lock, flags);

	client = dongle->clients[wcid - 1];
	if (client)
		err = gip_process_buffer(client->adapter, skb->data, skb->len);

	spin_unlock_irqrestore(&dongle->clients_lock, flags);

	return err;
}

static int xone_dongle_handle_association(struct xone_dongle *dongle, u8 *addr)
{
	struct xone_dongle_event *evt;

	evt = xone_dongle_alloc_event(dongle, XONE_DONGLE_EVT_ADD_CLIENT);
	if (!evt)
		return -ENOMEM;

	memcpy(evt->address, addr, ETH_ALEN);

	queue_work(dongle->event_wq, &evt->work);

	return 0;
}

static int xone_dongle_handle_disassociation(struct xone_dongle *dongle,
					     u8 wcid)
{
	struct xone_dongle_event *evt;

	if (!wcid || wcid > XONE_DONGLE_MAX_CLIENTS)
		return 0;

	evt = xone_dongle_alloc_event(dongle, XONE_DONGLE_EVT_REMOVE_CLIENT);
	if (!evt)
		return -ENOMEM;

	evt->wcid = wcid;

	queue_work(dongle->event_wq, &evt->work);

	return 0;
}

static int xone_dongle_handle_client_command(struct xone_dongle *dongle,
					     struct sk_buff *skb,
					     u8 wcid, u8 *addr)
{
	struct xone_dongle_event *evt;
	enum xone_dongle_event_type evt_type;

	if (skb->len < 2 || skb->data[0] != XONE_MT_WLAN_RESERVED)
		return -EINVAL;

	switch (skb->data[1]) {
	case XONE_MT_CLIENT_PAIR_REQ:
		evt_type = XONE_DONGLE_EVT_PAIR_CLIENT;
		break;
	case XONE_MT_CLIENT_ENABLE_ENCRYPTION:
		if (!wcid || wcid > XONE_DONGLE_MAX_CLIENTS)
			return -EINVAL;

		evt_type = XONE_DONGLE_EVT_ENABLE_ENCRYPTION;
		break;
	default:
		return 0;
	}

	evt = xone_dongle_alloc_event(dongle, evt_type);
	if (!evt)
		return -ENOMEM;

	evt->wcid = wcid;
	memcpy(evt->address, addr, ETH_ALEN);

	queue_work(dongle->event_wq, &evt->work);

	return 0;
}

static int xone_dongle_handle_button(struct xone_dongle *dongle)
{
	struct xone_dongle_event *evt;

	evt = xone_dongle_alloc_event(dongle, XONE_DONGLE_EVT_ENABLE_PAIRING);
	if (!evt)
		return -ENOMEM;

	queue_work(dongle->event_wq, &evt->work);

	return 0;
}

static int xone_dongle_handle_loss(struct xone_dongle *dongle,
				   struct sk_buff *skb)
{
	u8 wcid;

	if (skb->len < sizeof(wcid))
		return -EINVAL;

	wcid = skb->data[0];
	if (!wcid || wcid > XONE_DONGLE_MAX_CLIENTS)
		return 0;

	dev_dbg(dongle->mt.dev, "%s: wcid=%d\n", __func__, wcid);

	return xone_dongle_handle_disassociation(dongle, wcid);
}

static int xone_dongle_process_frame(struct xone_dongle *dongle,
				     struct sk_buff *skb,
				     unsigned int hdr_len, u8 wcid)
{
	struct ieee80211_hdr_3addr *hdr =
		(struct ieee80211_hdr_3addr *)skb->data;
	u16 type;

	/* ignore invalid frames */
	if (skb->len < hdr_len || hdr_len < sizeof(*hdr))
		return 0;

	skb_pull(skb, hdr_len);
	type = le16_to_cpu(hdr->frame_control);

	switch (type & (IEEE80211_FCTL_FTYPE | IEEE80211_FCTL_STYPE)) {
	case IEEE80211_FTYPE_DATA | IEEE80211_STYPE_QOS_DATA:
		return xone_dongle_handle_qos_data(dongle, skb, wcid);
	case IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_ASSOC_REQ:
		return xone_dongle_handle_association(dongle, hdr->addr2);
	case IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_DISASSOC:
		return xone_dongle_handle_disassociation(dongle, wcid);
	case IEEE80211_FTYPE_MGMT | XONE_MT_WLAN_RESERVED:
		return xone_dongle_handle_client_command(dongle, skb, wcid,
							 hdr->addr2);
	}

	return 0;
}

static int xone_dongle_process_wlan(struct xone_dongle *dongle,
				    struct sk_buff *skb)
{
	struct mt76_rxwi *rxwi = (struct mt76_rxwi *)skb->data;
	unsigned int hdr_len;
	u32 ctl;

	if (skb->len < sizeof(*rxwi))
		return -EINVAL;

	skb_pull(skb, sizeof(*rxwi));
	hdr_len = ieee80211_get_hdrlen_from_skb(skb);

	/* 2 bytes of padding after 802.11 header */
	if (rxwi->rxinfo & cpu_to_le32(MT_RXINFO_L2PAD)) {
		if (skb->len < hdr_len + 2)
			return -EINVAL;

		memmove(skb->data + 2, skb->data, hdr_len);
		skb_pull(skb, 2);
	}

	ctl = le32_to_cpu(rxwi->ctl);
	skb_trim(skb, FIELD_GET(MT_RXWI_CTL_MPDU_LEN, ctl));

	return xone_dongle_process_frame(dongle, skb, hdr_len,
					 FIELD_GET(MT_RXWI_CTL_WCID, ctl));
}

static int xone_dongle_process_message(struct xone_dongle *dongle,
				       struct sk_buff *skb)
{
	enum mt76_dma_msg_port port;
	u32 info;

	/* command header + trailer */
	if (skb->len < MT_CMD_HDR_LEN * 2)
		return -EINVAL;

	info = get_unaligned_le32(skb->data);
	port = FIELD_GET(MT_RX_FCE_INFO_D_PORT, info);

	/* ignore command reponses */
	if (FIELD_GET(MT_RX_FCE_INFO_CMD_SEQ, info) == 0x01)
		return 0;

	/* remove header + trailer */
	skb_pull(skb, MT_CMD_HDR_LEN);
	skb_trim(skb, skb->len - MT_CMD_HDR_LEN);

	if (port == MT_WLAN_PORT)
		return xone_dongle_process_wlan(dongle, skb);

	if (port != MT_CPU_RX_PORT)
		return 0;

	switch (FIELD_GET(MT_RX_FCE_INFO_EVT_TYPE, info)) {
	case XONE_MT_EVT_BUTTON:
		return xone_dongle_handle_button(dongle);
	case XONE_MT_EVT_PACKET_RX:
		return xone_dongle_process_wlan(dongle, skb);
	case XONE_MT_EVT_CLIENT_LOST:
		return xone_dongle_handle_loss(dongle, skb);
	}

	return 0;
}

static int xone_dongle_process_buffer(struct xone_dongle *dongle,
				      void *data, int len)
{
	struct sk_buff *skb;
	int err;

	if (!len)
		return 0;

	skb = dev_alloc_skb(len);
	if (!skb)
		return -ENOMEM;

	skb_put_data(skb, data, len);

	err = xone_dongle_process_message(dongle, skb);
	if (err) {
		dev_err(dongle->mt.dev, "%s: process failed: %d\n",
			__func__, err);
		print_hex_dump_debug("xone-dongle packet: ", DUMP_PREFIX_NONE,
				     16, 1, data, len, false);
	}

	dev_kfree_skb(skb);

	return err;
}

static void xone_dongle_complete_in(struct urb *urb)
{
	struct xone_dongle *dongle = urb->context;
	int err;

	switch (urb->status) {
	case 0:
		break;
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
		usb_anchor_urb(urb, &dongle->urbs_in_idle);
		return;
	default:
		goto resubmit;
	}

	err = xone_dongle_process_buffer(dongle, urb->transfer_buffer,
					 urb->actual_length);
	if (err)
		dev_err(dongle->mt.dev, "%s: process failed: %d\n",
			__func__, err);

resubmit:
	/* can fail during USB device removal */
	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err) {
		dev_dbg(dongle->mt.dev, "%s: submit failed: %d\n",
			__func__, err);
		usb_anchor_urb(urb, &dongle->urbs_in_idle);
	} else {
		usb_anchor_urb(urb, &dongle->urbs_in_busy);
	}
}

static void xone_dongle_complete_out(struct urb *urb)
{
	struct sk_buff *skb = urb->context;
	struct xone_dongle_skb_cb *cb = (struct xone_dongle_skb_cb *)skb->cb;

	usb_anchor_urb(urb, &cb->dongle->urbs_out_idle);
	dev_consume_skb_any(skb);
}

static int xone_dongle_init_urbs_in(struct xone_dongle *dongle,
				    int ep, int buf_len)
{
	struct xone_mt76 *mt = &dongle->mt;
	struct urb *urb;
	void *buf;
	int i, err;

	for (i = 0; i < XONE_DONGLE_NUM_IN_URBS; i++) {
		urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!urb)
			return -ENOMEM;

		usb_anchor_urb(urb, &dongle->urbs_in_busy);
		usb_free_urb(urb);

		buf = usb_alloc_coherent(mt->udev, buf_len,
					 GFP_KERNEL, &urb->transfer_dma);
		if (!buf)
			return -ENOMEM;

		usb_fill_bulk_urb(urb, mt->udev,
				  usb_rcvbulkpipe(mt->udev, ep), buf, buf_len,
				  xone_dongle_complete_in, dongle);
		urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

		err = usb_submit_urb(urb, GFP_KERNEL);
		if (err)
			return err;
	}

	return 0;
}

static int xone_dongle_init_urbs_out(struct xone_dongle *dongle)
{
	struct xone_mt76 *mt = &dongle->mt;
	struct urb *urb;
	int i;

	for (i = 0; i < XONE_DONGLE_NUM_OUT_URBS; i++) {
		urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!urb)
			return -ENOMEM;

		usb_fill_bulk_urb(urb, mt->udev,
				  usb_sndbulkpipe(mt->udev, XONE_MT_EP_OUT),
				  NULL, 0, xone_dongle_complete_out, NULL);
		usb_anchor_urb(urb, &dongle->urbs_out_idle);
		usb_free_urb(urb);
	}

	return 0;
}

static int xone_dongle_init(struct xone_dongle *dongle)
{
	struct xone_mt76 *mt = &dongle->mt;
	int err;

	init_usb_anchor(&dongle->urbs_out_idle);
	init_usb_anchor(&dongle->urbs_out_busy);
	init_usb_anchor(&dongle->urbs_in_idle);
	init_usb_anchor(&dongle->urbs_in_busy);

	err = xone_dongle_init_urbs_out(dongle);
	if (err)
		return err;

	err = xone_dongle_init_urbs_in(dongle, XONE_MT_EP_IN_CMD,
				       XONE_DONGLE_LEN_CMD_PKT);
	if (err)
		return err;

	err = xone_dongle_init_urbs_in(dongle, XONE_MT_EP_IN_WLAN,
				       XONE_DONGLE_LEN_WLAN_PKT);
	if (err)
		return err;

	err = xone_mt76_load_firmware(mt, "xow_dongle.bin");
	if (err) {
		dev_err(mt->dev, "%s: load firmware failed: %d\n",
			__func__, err);
		return err;
	}

	err = xone_mt76_init_radio(mt);
	if (err)
		dev_err(mt->dev, "%s: init radio failed: %d\n", __func__, err);

	return err;
}

static int xone_dongle_power_off_clients(struct xone_dongle *dongle)
{
	struct xone_dongle_client *client;
	int i;
	int err = 0;
	unsigned long flags;

	spin_lock_irqsave(&dongle->clients_lock, flags);

	for (i = 0; i < XONE_DONGLE_MAX_CLIENTS; i++) {
		client = dongle->clients[i];
		if (!client)
			continue;

		err = gip_power_off_adapter(client->adapter);
		if (err)
			break;
	}

	spin_unlock_irqrestore(&dongle->clients_lock, flags);

	if (err)
		return err;

	/* can time out if new client connects */
	if (!wait_event_timeout(dongle->disconnect_wait,
				!atomic_read(&dongle->client_count),
				XONE_DONGLE_PWR_OFF_TIMEOUT))
		return -ETIMEDOUT;

	return xone_dongle_toggle_pairing(dongle, false);
}

static void xone_dongle_destroy(struct xone_dongle *dongle)
{
	struct xone_dongle_client *client;
	struct urb *urb;
	int i;

	usb_kill_anchored_urbs(&dongle->urbs_in_busy);
	destroy_workqueue(dongle->event_wq);
	cancel_delayed_work_sync(&dongle->pairing_work);

	for (i = 0; i < XONE_DONGLE_MAX_CLIENTS; i++) {
		client = dongle->clients[i];
		if (!client)
			continue;

		gip_destroy_adapter(client->adapter);
		kfree(client);
		dongle->clients[i] = NULL;
	}

	usb_kill_anchored_urbs(&dongle->urbs_out_busy);

	while ((urb = usb_get_from_anchor(&dongle->urbs_out_idle)))
		usb_free_urb(urb);

	while ((urb = usb_get_from_anchor(&dongle->urbs_in_idle))) {
		usb_free_coherent(urb->dev, urb->transfer_buffer_length,
				  urb->transfer_buffer, urb->transfer_dma);
		usb_free_urb(urb);
	}

	mutex_destroy(&dongle->pairing_lock);
}

static int xone_dongle_probe(struct usb_interface *intf,
			     const struct usb_device_id *id)
{
	struct xone_dongle *dongle;
	int err;

	dongle = devm_kzalloc(&intf->dev, sizeof(*dongle), GFP_KERNEL);
	if (!dongle)
		return -ENOMEM;

	dongle->mt.dev = &intf->dev;
	dongle->mt.udev = interface_to_usbdev(intf);

	usb_reset_device(dongle->mt.udev);

	dongle->event_wq = alloc_ordered_workqueue("xone_dongle", 0);
	if (!dongle->event_wq)
		return -ENOMEM;

	mutex_init(&dongle->pairing_lock);
	INIT_DELAYED_WORK(&dongle->pairing_work, xone_dongle_pairing_timeout);
	spin_lock_init(&dongle->clients_lock);
	init_waitqueue_head(&dongle->disconnect_wait);

	err = xone_dongle_init(dongle);
	if (err) {
		xone_dongle_destroy(dongle);
		return err;
	}

	usb_set_intfdata(intf, dongle);

	err = device_add_groups(&intf->dev, xone_dongle_groups);
	if (err) {
		xone_dongle_destroy(dongle);
		return err;
	}

	/* enable USB remote wakeup and autosuspend */
	intf->needs_remote_wakeup = true;
	device_wakeup_enable(&dongle->mt.udev->dev);
	pm_runtime_set_autosuspend_delay(&dongle->mt.udev->dev,
					 XONE_DONGLE_SUSPEND_DELAY);
	usb_enable_autosuspend(dongle->mt.udev);

	return 0;
}

static void xone_dongle_disconnect(struct usb_interface *intf)
{
	struct xone_dongle *dongle = usb_get_intfdata(intf);
	int err;

	device_remove_groups(&intf->dev, xone_dongle_groups);

	/* can fail during USB device removal */
	err = xone_dongle_power_off_clients(dongle);
	if (err)
		dev_dbg(dongle->mt.dev, "%s: power off failed: %d\n",
			__func__, err);

	xone_dongle_destroy(dongle);
	usb_set_intfdata(intf, NULL);
}

static int xone_dongle_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct xone_dongle *dongle = usb_get_intfdata(intf);
	int err;

	err = xone_dongle_power_off_clients(dongle);
	if (err)
		dev_err(dongle->mt.dev, "%s: power off failed: %d\n",
			__func__, err);

	usb_kill_anchored_urbs(&dongle->urbs_in_busy);
	usb_kill_anchored_urbs(&dongle->urbs_out_busy);
	cancel_delayed_work_sync(&dongle->pairing_work);

	return xone_mt76_suspend_radio(&dongle->mt);
}

static int xone_dongle_resume(struct usb_interface *intf)
{
	struct xone_dongle *dongle = usb_get_intfdata(intf);
	struct urb *urb;
	int err;

	while ((urb = usb_get_from_anchor(&dongle->urbs_in_idle))) {
		usb_anchor_urb(urb, &dongle->urbs_in_busy);
		usb_free_urb(urb);

		err = usb_submit_urb(urb, GFP_KERNEL);
		if (err)
			return err;
	}

	return xone_mt76_resume_radio(&dongle->mt);
}

static void xone_dongle_shutdown(struct device *dev)
{
	struct usb_interface *intf = to_usb_interface(dev);
	struct xone_dongle *dongle = usb_get_intfdata(intf);
	int err;

	err = xone_dongle_power_off_clients(dongle);
	if (err)
		dev_err(dongle->mt.dev, "%s: power off failed: %d\n",
			__func__, err);
}

static const struct usb_device_id xone_dongle_id_table[] = {
	{ USB_DEVICE(0x045e, 0x02e6) }, /* old dongle */
	{ USB_DEVICE(0x045e, 0x02fe) }, /* new dongle */
	{ USB_DEVICE(0x045e, 0x02f9) }, /* built-in dongle (ASUS, Lenovo) */
	{ USB_DEVICE(0x045e, 0x091e) }, /* built-in dongle (Surface Book 2) */
	{ },
};

static struct usb_driver xone_dongle_driver = {
	.name = "xone-dongle",
	.probe = xone_dongle_probe,
	.disconnect = xone_dongle_disconnect,
	.suspend = xone_dongle_suspend,
	.resume = xone_dongle_resume,
	.id_table = xone_dongle_id_table,
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 0)
	.drvwrap.driver.shutdown = xone_dongle_shutdown,
#else
	.driver.shutdown = xone_dongle_shutdown,
#endif
	.supports_autosuspend = true,
	.disable_hub_initiated_lpm = true,
	.soft_unbind = true,
};

module_usb_driver(xone_dongle_driver);

MODULE_DEVICE_TABLE(usb, xone_dongle_id_table);
MODULE_AUTHOR("Severin von Wnuck-Lipinski <severinvonw@outlook.de>");
MODULE_DESCRIPTION("xone dongle driver");
MODULE_VERSION("#VERSION#");
MODULE_LICENSE("GPL");
