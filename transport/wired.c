// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2021 Severin von Wnuck-Lipinski <severinvonw@outlook.de>
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>

#include "../bus/bus.h"

#define XONE_WIRED_INTF_DATA 0
#define XONE_WIRED_INTF_AUDIO 1

#define XONE_WIRED_NUM_DATA_URBS 8
#define XONE_WIRED_NUM_AUDIO_URBS 12
#define XONE_WIRED_NUM_AUDIO_PKTS 8

#define XONE_WIRED_LEN_DATA_PKT 64

#define XONE_WIRED_VENDOR(vendor) \
	.match_flags = USB_DEVICE_ID_MATCH_VENDOR | \
		       USB_DEVICE_ID_MATCH_INT_INFO | \
		       USB_DEVICE_ID_MATCH_INT_NUMBER, \
	.idVendor = vendor, \
	.bInterfaceClass = USB_CLASS_VENDOR_SPEC, \
	.bInterfaceSubClass = 0x47, \
	.bInterfaceProtocol = 0xd0, \
	.bInterfaceNumber = XONE_WIRED_INTF_DATA,

struct xone_wired {
	struct usb_device *udev;

	struct xone_wired_port {
		struct device *dev;

		struct usb_endpoint_descriptor *ep_in;
		struct usb_endpoint_descriptor *ep_out;

		struct urb *urb_in;
		struct usb_anchor urbs_out_idle;
		struct usb_anchor urbs_out_busy;

		int buffer_length_out;
	} data_port, audio_port;

	struct gip_adapter *adapter;
};

static void xone_wired_complete_data_in(struct urb *urb)
{
	struct xone_wired *wired = urb->context;
	struct device *dev = wired->data_port.dev;
	int err;

	switch (urb->status) {
	case 0:
		break;
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
		return;
	default:
		goto resubmit;
	}

	if (!urb->actual_length)
		goto resubmit;

	err = gip_process_buffer(wired->adapter, urb->transfer_buffer,
				 urb->actual_length);
	if (err) {
		dev_err(dev, "%s: process failed: %d\n", __func__, err);
		print_hex_dump_debug("xone-wired packet: ",
				     DUMP_PREFIX_NONE, 16, 1,
				     urb->transfer_buffer, urb->actual_length,
				     false);
	}

resubmit:
	/* can fail during USB device removal */
	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err)
		dev_dbg(dev, "%s: submit failed: %d\n", __func__, err);
}

static void xone_wired_complete_audio_in(struct urb *urb)
{
	struct xone_wired *wired = urb->context;
	struct device *dev = wired->audio_port.dev;
	struct usb_iso_packet_descriptor *desc;
	int i, err;

	if (urb->status)
		return;

	for (i = 0; i < urb->number_of_packets; i++) {
		desc = &urb->iso_frame_desc[i];

		/* device reset after system sleep can cause xHCI errors */
		if (desc->status == -EPROTO) {
			dev_warn_once(dev, "%s: protocol error\n", __func__);
			break;
		}

		if (!desc->actual_length)
			continue;

		err = gip_process_buffer(wired->adapter,
					 urb->transfer_buffer + desc->offset,
					 desc->actual_length);
		if (err)
			dev_err(dev, "%s: process failed: %d\n", __func__, err);
	}

	/* can fail during USB device removal */
	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err)
		dev_dbg(dev, "%s: submit failed: %d\n", __func__, err);
}

static void xone_wired_complete_out(struct urb *urb)
{
	struct xone_wired_port *port = urb->context;

	usb_anchor_urb(urb, &port->urbs_out_idle);
}

static int xone_wired_init_data_in(struct xone_wired *wired)
{
	struct xone_wired_port *port = &wired->data_port;
	struct urb *urb;
	void *buf;

	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb)
		return -ENOMEM;

	port->urb_in = urb;

	buf = usb_alloc_coherent(wired->udev, XONE_WIRED_LEN_DATA_PKT,
				 GFP_KERNEL, &urb->transfer_dma);
	if (!buf)
		return -ENOMEM;

	usb_fill_int_urb(urb, wired->udev,
			 usb_rcvintpipe(wired->udev,
					port->ep_in->bEndpointAddress),
			 buf, XONE_WIRED_LEN_DATA_PKT,
			 xone_wired_complete_data_in, wired,
			 port->ep_in->bInterval);
	urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	return usb_submit_urb(urb, GFP_KERNEL);
}

static int xone_wired_init_data_out(struct xone_wired *wired)
{
	struct xone_wired_port *port = &wired->data_port;
	struct urb *urb;
	void *buf;
	int i;

	port->buffer_length_out = XONE_WIRED_LEN_DATA_PKT;

	for (i = 0; i < XONE_WIRED_NUM_DATA_URBS; i++) {
		urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!urb)
			return -ENOMEM;

		usb_anchor_urb(urb, &port->urbs_out_idle);
		usb_free_urb(urb);

		buf = usb_alloc_coherent(wired->udev, XONE_WIRED_LEN_DATA_PKT,
					 GFP_KERNEL, &urb->transfer_dma);
		if (!buf)
			return -ENOMEM;

		usb_fill_int_urb(urb, wired->udev,
				 usb_sndintpipe(wired->udev,
						port->ep_out->bEndpointAddress),
				 buf, XONE_WIRED_LEN_DATA_PKT,
				 xone_wired_complete_out, port,
				 port->ep_out->bInterval);
		urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	}

	return 0;
}

static void xone_wired_free_urbs(struct xone_wired_port *port)
{
	struct urb *urb = port->urb_in;

	if (urb) {
		usb_free_coherent(urb->dev, urb->transfer_buffer_length,
				  urb->transfer_buffer, urb->transfer_dma);
		usb_free_urb(urb);
		port->urb_in = NULL;
	}

	while ((urb = usb_get_from_anchor(&port->urbs_out_idle))) {
		usb_free_coherent(urb->dev, port->buffer_length_out,
				  urb->transfer_buffer, urb->transfer_dma);
		usb_free_urb(urb);
	}
}

static int xone_wired_get_buffer(struct gip_adapter *adap,
				 struct gip_adapter_buffer *buf)
{
	struct xone_wired *wired = dev_get_drvdata(&adap->dev);
	struct xone_wired_port *port;
	struct urb *urb;

	if (buf->type == GIP_BUF_DATA)
		port = &wired->data_port;
	else if (buf->type == GIP_BUF_AUDIO)
		port = &wired->audio_port;
	else
		return -EINVAL;

	urb = usb_get_from_anchor(&port->urbs_out_idle);
	if (!urb)
		return -ENOSPC;

	buf->context = urb;
	buf->data = urb->transfer_buffer;
	buf->length = port->buffer_length_out;

	return 0;
}

static int xone_wired_submit_buffer(struct gip_adapter *adap,
				    struct gip_adapter_buffer *buf)
{
	struct xone_wired *wired = dev_get_drvdata(&adap->dev);
	struct xone_wired_port *port;
	struct urb *urb = buf->context;
	int err;

	if (buf->type == GIP_BUF_DATA)
		port = &wired->data_port;
	else if (buf->type == GIP_BUF_AUDIO)
		port = &wired->audio_port;
	else
		return -EINVAL;

	urb->transfer_buffer_length = buf->length;
	usb_anchor_urb(urb, &port->urbs_out_busy);

	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err) {
		usb_unanchor_urb(urb);
		usb_anchor_urb(urb, &port->urbs_out_idle);
	}

	usb_free_urb(urb);

	return err;
}

static int xone_wired_enable_audio(struct gip_adapter *adap)
{
	struct xone_wired *wired = dev_get_drvdata(&adap->dev);
	struct usb_interface *intf;

	if (!wired->audio_port.dev)
		return -ENOTSUPP;

	intf = to_usb_interface(wired->audio_port.dev);
	if (intf->cur_altsetting->desc.bAlternateSetting == 1)
		return -EALREADY;

	return usb_set_interface(wired->udev, XONE_WIRED_INTF_AUDIO, 1);
}

static int xone_wired_init_audio_in(struct gip_adapter *adap)
{
	struct xone_wired *wired = dev_get_drvdata(&adap->dev);
	struct xone_wired_port *port = &wired->audio_port;
	struct urb *urb;
	void *buf;
	int len, i;

	if (!port->ep_in)
		return -ENOTSUPP;

	urb = usb_alloc_urb(XONE_WIRED_NUM_AUDIO_PKTS, GFP_KERNEL);
	if (!urb)
		return -ENOMEM;

	port->urb_in = urb;

	len = usb_endpoint_maxp(port->ep_in);
	buf = usb_alloc_coherent(wired->udev, len * XONE_WIRED_NUM_AUDIO_PKTS,
				 GFP_KERNEL, &urb->transfer_dma);
	if (!buf)
		return -ENOMEM;

	urb->dev = wired->udev;
	urb->pipe = usb_rcvisocpipe(wired->udev, port->ep_in->bEndpointAddress);
	urb->transfer_flags = URB_ISO_ASAP | URB_NO_TRANSFER_DMA_MAP;
	urb->transfer_buffer = buf;
	urb->transfer_buffer_length = len * XONE_WIRED_NUM_AUDIO_PKTS;
	urb->number_of_packets = XONE_WIRED_NUM_AUDIO_PKTS;
	urb->interval = port->ep_in->bInterval;
	urb->context = wired;
	urb->complete = xone_wired_complete_audio_in;

	for (i = 0; i < XONE_WIRED_NUM_AUDIO_PKTS; i++) {
		urb->iso_frame_desc[i].offset = i * len;
		urb->iso_frame_desc[i].length = len;
	}

	return usb_submit_urb(urb, GFP_KERNEL);
}

static int xone_wired_init_audio_out(struct gip_adapter *adap, int pkt_len)
{
	struct xone_wired *wired = dev_get_drvdata(&adap->dev);
	struct xone_wired_port *port = &wired->audio_port;
	struct urb *urb;
	void *buf;
	int i, j;

	if (!port->ep_out)
		return -ENOTSUPP;

	port->buffer_length_out = pkt_len * XONE_WIRED_NUM_AUDIO_PKTS;

	for (i = 0; i < XONE_WIRED_NUM_AUDIO_URBS; i++) {
		urb = usb_alloc_urb(XONE_WIRED_NUM_AUDIO_PKTS, GFP_KERNEL);
		if (!urb)
			return -ENOMEM;

		usb_anchor_urb(urb, &port->urbs_out_idle);
		usb_free_urb(urb);

		buf = usb_alloc_coherent(wired->udev, port->buffer_length_out,
					 GFP_KERNEL, &urb->transfer_dma);
		if (!buf)
			return -ENOMEM;

		urb->dev = wired->udev;
		urb->pipe = usb_sndisocpipe(wired->udev,
					    port->ep_out->bEndpointAddress);
		urb->transfer_flags = URB_ISO_ASAP | URB_NO_TRANSFER_DMA_MAP;
		urb->transfer_buffer = buf;
		urb->transfer_buffer_length = port->buffer_length_out;
		urb->number_of_packets = XONE_WIRED_NUM_AUDIO_PKTS;
		urb->interval = port->ep_out->bInterval;
		urb->context = port;
		urb->complete = xone_wired_complete_out;

		for (j = 0; j < XONE_WIRED_NUM_AUDIO_PKTS; j++) {
			urb->iso_frame_desc[j].offset = j * pkt_len;
			urb->iso_frame_desc[j].length = pkt_len;
		}
	}

	return 0;
}

static int xone_wired_disable_audio(struct gip_adapter *adap)
{
	struct xone_wired *wired = dev_get_drvdata(&adap->dev);
	struct xone_wired_port *port = &wired->audio_port;
	struct usb_interface *intf;

	if (!port->dev)
		return -ENOTSUPP;

	intf = to_usb_interface(port->dev);
	if (!intf->cur_altsetting->desc.bAlternateSetting)
		return -EALREADY;

	usb_kill_urb(port->urb_in);
	usb_kill_anchored_urbs(&port->urbs_out_busy);
	xone_wired_free_urbs(port);

	return usb_set_interface(wired->udev, XONE_WIRED_INTF_AUDIO, 0);
}

static struct gip_adapter_ops xone_wired_adapter_ops = {
	.get_buffer = xone_wired_get_buffer,
	.submit_buffer = xone_wired_submit_buffer,
	.enable_audio = xone_wired_enable_audio,
	.init_audio_in = xone_wired_init_audio_in,
	.init_audio_out = xone_wired_init_audio_out,
	.disable_audio = xone_wired_disable_audio,
};

static struct usb_driver xone_wired_driver;

static int xone_wired_find_isoc_endpoints(struct usb_host_interface *alt,
					  struct usb_endpoint_descriptor **in,
					  struct usb_endpoint_descriptor **out)
{
	struct usb_endpoint_descriptor *ep;
	int i;

	for (i = 0; i < alt->desc.bNumEndpoints; i++) {
		ep = &alt->endpoint[i].desc;
		if (usb_endpoint_is_isoc_in(ep))
			*in = ep;
		else if (usb_endpoint_is_isoc_out(ep))
			*out = ep;

		if (*in && *out)
			return 0;
	}

	return -ENXIO;
}

static int xone_wired_init_data_port(struct xone_wired *wired,
				     struct usb_interface *intf)
{
	struct xone_wired_port *port = &wired->data_port;
	int err;

	init_usb_anchor(&port->urbs_out_idle);
	init_usb_anchor(&port->urbs_out_busy);

	err = usb_find_common_endpoints(intf->cur_altsetting, NULL, NULL,
					&port->ep_in, &port->ep_out);
	if (err)
		return err;

	port->dev = &intf->dev;

	return 0;
}

static int xone_wired_init_audio_port(struct xone_wired *wired)
{
	struct xone_wired_port *port = &wired->audio_port;
	struct usb_interface *intf;
	struct usb_host_interface *alt;
	int err;

	init_usb_anchor(&port->urbs_out_idle);
	init_usb_anchor(&port->urbs_out_busy);

	intf = usb_ifnum_to_if(wired->udev, XONE_WIRED_INTF_AUDIO);
	if (!intf) {
		dev_dbg(&wired->udev->dev, "%s: audio unavailable\n", __func__);
		return 0;
	}

	alt = usb_altnum_to_altsetting(intf, 1);
	if (!alt)
		return -ENXIO;

	err = usb_driver_claim_interface(&xone_wired_driver, intf, NULL);
	if (err)
		return err;

	/* disable the audio interface */
	/* mandatory for certain third party devices */
	err = usb_set_interface(wired->udev, XONE_WIRED_INTF_AUDIO, 0);
	if (err)
		return err;

	err = xone_wired_find_isoc_endpoints(alt, &port->ep_in, &port->ep_out);
	if (err)
		return err;

	port->dev = &intf->dev;

	return 0;
}

static int xone_wired_probe(struct usb_interface *intf,
			    const struct usb_device_id *id)
{
	struct xone_wired *wired;
	int err;

	wired = devm_kzalloc(&intf->dev, sizeof(*wired), GFP_KERNEL);
	if (!wired)
		return -ENOMEM;

	wired->udev = interface_to_usbdev(intf);

	/* newer devices require a reset after system sleep */
	usb_reset_device(wired->udev);

	err = xone_wired_init_data_port(wired, intf);
	if (err)
		return err;

	err = xone_wired_init_audio_port(wired);
	if (err)
		return err;

	wired->adapter = gip_create_adapter(&intf->dev, &xone_wired_adapter_ops,
					    XONE_WIRED_NUM_AUDIO_PKTS);
	if (IS_ERR(wired->adapter))
		return PTR_ERR(wired->adapter);

	dev_set_drvdata(&wired->adapter->dev, wired);

	err = xone_wired_init_data_out(wired);
	if (err)
		goto err_free_urbs;

	err = xone_wired_init_data_in(wired);
	if (err)
		goto err_free_urbs;

	usb_set_intfdata(intf, wired);

	/* enable USB remote wakeup */
	device_wakeup_enable(&wired->udev->dev);

	return 0;

err_free_urbs:
	xone_wired_free_urbs(&wired->data_port);
	gip_destroy_adapter(wired->adapter);

	return err;
}

static void xone_wired_disconnect(struct usb_interface *intf)
{
	struct xone_wired *wired = usb_get_intfdata(intf);

	/* ignore audio interface unbind */
	if (!wired)
		return;

	usb_kill_urb(wired->data_port.urb_in);
	usb_kill_urb(wired->audio_port.urb_in);

	/* also disables the audio interface */
	gip_destroy_adapter(wired->adapter);

	usb_kill_anchored_urbs(&wired->data_port.urbs_out_busy);
	xone_wired_free_urbs(&wired->data_port);

	usb_set_intfdata(intf, NULL);
}

static const struct usb_device_id xone_wired_id_table[] = {
	{ XONE_WIRED_VENDOR(0x045e) }, /* Microsoft */
	{ XONE_WIRED_VENDOR(0x0738) }, /* Mad Catz */
	{ XONE_WIRED_VENDOR(0x0e6f) }, /* PDP */
	{ XONE_WIRED_VENDOR(0x0f0d) }, /* Hori */
	{ XONE_WIRED_VENDOR(0x1532) }, /* Razer */
	{ XONE_WIRED_VENDOR(0x24c6) }, /* PowerA */
	{ XONE_WIRED_VENDOR(0x20d6) }, /* BDA */
	{ XONE_WIRED_VENDOR(0x044f) }, /* Thrustmaster */
	{ XONE_WIRED_VENDOR(0x10f5) }, /* Turtle Beach */
	{ XONE_WIRED_VENDOR(0x2e24) }, /* Hyperkin */
	{ XONE_WIRED_VENDOR(0x3285) }, /* Nacon */
	{ XONE_WIRED_VENDOR(0x2dc8) }, /* 8BitDo */
	{ XONE_WIRED_VENDOR(0x2e95) }, /* SCUF */
	{ XONE_WIRED_VENDOR(0x3537) }, /* GameSir */
	{ XONE_WIRED_VENDOR(0x11c1) }, /* ??? */
	{ XONE_WIRED_VENDOR(0x294b) }, /* Snakebyte */
	{ XONE_WIRED_VENDOR(0x2c16) }, /* Priferential */
	{ XONE_WIRED_VENDOR(0x0b05) }, /* ASUS */
	{ XONE_WIRED_VENDOR(0x413d) }, /* BIGBIG WON */
	{ },
};

static struct usb_driver xone_wired_driver = {
	.name = "xone-wired",
	.probe = xone_wired_probe,
	.disconnect = xone_wired_disconnect,
	.id_table = xone_wired_id_table,
};

module_usb_driver(xone_wired_driver);

MODULE_DEVICE_TABLE(usb, xone_wired_id_table);
MODULE_AUTHOR("Severin von Wnuck-Lipinski <severinvonw@outlook.de>");
MODULE_DESCRIPTION("xone wired driver");
MODULE_VERSION("#VERSION#");
MODULE_LICENSE("GPL");
