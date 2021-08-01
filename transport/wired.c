// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2021 Severin von Wnuck <severinvonw@outlook.de>
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>

#include "../bus/bus.h"

#define XONE_WIRED_NUM_DATA_URBS 8

#define XONE_WIRED_NUM_AUDIO_URBS 12
#define XONE_WIRED_NUM_AUDIO_PKTS 8

#define XONE_WIRED_VENDOR(vendor) \
	.match_flags = USB_DEVICE_ID_MATCH_VENDOR | \
			USB_DEVICE_ID_MATCH_INT_INFO | \
			USB_DEVICE_ID_MATCH_INT_NUMBER, \
	.idVendor = (vendor), \
	.bInterfaceClass = USB_CLASS_VENDOR_SPEC, \
	.bInterfaceSubClass = 0x47, \
	.bInterfaceProtocol = 0xd0, \
	.bInterfaceNumber = 0x00,

struct xone_wired {
	struct usb_device *udev;

	struct xone_wired_port {
		struct usb_interface *intf;

		struct usb_endpoint_descriptor *ep_in;
		struct usb_endpoint_descriptor *ep_out;

		struct urb *urb_in;
		struct usb_anchor urbs_out_idle;
		struct usb_anchor urbs_out_busy;

		int buffer_length_in;
		int buffer_length_out;
	} data_port, audio_port;

	struct gip_adapter *adapter;
};

static void xone_wired_data_in_complete(struct urb *urb)
{
	struct xone_wired *xone = urb->context;
	struct device *dev = &xone->data_port.intf->dev;
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

	if (urb->actual_length) {
		err = gip_process_buffer(xone->adapter,
				urb->transfer_buffer, urb->actual_length);
		if (err)
			dev_err(dev, "%s: process failed: %d\n", __func__, err);
	}

resubmit:
	/* can fail during USB device removal */
	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err)
		dev_dbg(dev, "%s: submit failed: %d\n", __func__, err);
}

static void xone_wired_audio_in_complete(struct urb *urb)
{
	struct xone_wired *xone = urb->context;
	struct device *dev = &xone->audio_port.intf->dev;
	struct usb_iso_packet_descriptor *desc;
	int i, err;

	if (urb->status)
		return;

	for (i = 0; i < urb->number_of_packets; i++) {
		desc = &urb->iso_frame_desc[i];
		if (!desc->actual_length)
			continue;

		err = gip_process_buffer(xone->adapter,
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

static void xone_wired_out_complete(struct urb *urb)
{
	struct xone_wired_port *port = urb->context;

	usb_anchor_urb(urb, &port->urbs_out_idle);
}

static int xone_wired_init_data_in(struct xone_wired *xone)
{
	struct xone_wired_port *port = &xone->data_port;
	struct urb *urb;
	int len;
	void *buf;

	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb)
		return -ENOMEM;

	len = usb_endpoint_maxp(port->ep_in);
	buf = usb_alloc_coherent(xone->udev, len, GFP_KERNEL,
			&urb->transfer_dma);
	if (!buf)
		return -ENOMEM;

	usb_fill_int_urb(urb, xone->udev,
			usb_rcvintpipe(xone->udev, port->ep_in->bEndpointAddress),
			buf, len, xone_wired_data_in_complete,
			xone, port->ep_in->bInterval);
	urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	port->urb_in = urb;
	port->buffer_length_in = len;

	return 0;
}

static int xone_wired_init_data_out(struct xone_wired *xone)
{
	struct xone_wired_port *port = &xone->data_port;
	struct urb *urb;
	void *buf;
	int len, i;

	len = usb_endpoint_maxp(port->ep_out);

	for (i = 0; i < XONE_WIRED_NUM_DATA_URBS; i++) {
		urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!urb)
			return -ENOMEM;

		buf = usb_alloc_coherent(xone->udev, len, GFP_KERNEL,
				&urb->transfer_dma);
		if (!buf)
			return -ENOMEM;

		usb_fill_int_urb(urb, xone->udev,
				usb_sndintpipe(xone->udev, port->ep_out->bEndpointAddress),
				buf, len, xone_wired_out_complete,
				port, port->ep_out->bInterval);
		urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

		usb_anchor_urb(urb, &port->urbs_out_idle);
	}

	port->buffer_length_out = len;

	return 0;
}

static void xone_wired_free_port(struct xone_wired_port *port)
{
	struct urb *urb;

	if (port->urb_in) {
		usb_free_coherent(port->urb_in->dev, port->buffer_length_in,
			port->urb_in->transfer_buffer,
			port->urb_in->transfer_dma);
		usb_free_urb(port->urb_in);
	}

	while ((urb = usb_get_from_anchor(&port->urbs_out_idle))) {
		usb_free_coherent(urb->dev, port->buffer_length_out,
				urb->transfer_buffer,
				urb->transfer_dma);
		usb_free_urb(urb);
	}

	port->intf = NULL;
	port->ep_in = NULL;
	port->ep_out = NULL;
	port->urb_in = NULL;
}

static int xone_wired_get_buffer(struct gip_adapter *adap,
		struct gip_adapter_buffer *buf)
{
	struct xone_wired *xone = dev_get_drvdata(&adap->dev);
	struct xone_wired_port *port;
	struct urb *urb;

	if (buf->type == GIP_BUF_DATA)
		port = &xone->data_port;
	else if (buf->type == GIP_BUF_AUDIO)
		port = &xone->audio_port;
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
	struct xone_wired *xone = dev_get_drvdata(&adap->dev);
	struct xone_wired_port *port;
	struct urb *urb = buf->context;
	int err;

	if (buf->type == GIP_BUF_DATA)
		port = &xone->data_port;
	else if (buf->type == GIP_BUF_AUDIO)
		port = &xone->audio_port;
	else
		return -EINVAL;

	urb->transfer_buffer_length = buf->length;
	usb_anchor_urb(urb, &port->urbs_out_busy);

	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err)
		usb_unanchor_urb(urb);

	usb_free_urb(urb);

	return err;
}

static int xone_wired_enable_audio(struct gip_adapter *adap)
{
	struct xone_wired *xone = dev_get_drvdata(&adap->dev);
	struct xone_wired_port *port = &xone->audio_port;
	struct usb_interface *intf;
	struct usb_endpoint_descriptor *ep, *ep_in, *ep_out;
	int i, err;

	intf = usb_ifnum_to_if(xone->udev, 1);
	if (!intf)
		return -ENODEV;

	if (intf->cur_altsetting->desc.bAlternateSetting == 1)
		return -EALREADY;

	err = usb_set_interface(xone->udev, 1, 1);
	if (err)
		return err;

	for (i = 0; i < intf->cur_altsetting->desc.bNumEndpoints; i++) {
		ep = &intf->cur_altsetting->endpoint[i].desc;
		if (usb_endpoint_is_isoc_in(ep))
			ep_in = ep;
		else if (usb_endpoint_is_isoc_out(ep))
			ep_out = ep;
	}

	if (!ep_in || !ep_out)
		return -ENODEV;

	port->intf = intf;
	port->ep_in = ep_in;
	port->ep_out = ep_out;

	return 0;
}

static int xone_wired_init_audio_in(struct gip_adapter *adap)
{
	struct xone_wired *xone = dev_get_drvdata(&adap->dev);
	struct xone_wired_port *port = &xone->audio_port;
	struct urb *urb;
	void *buf;
	int len, i;

	urb = usb_alloc_urb(XONE_WIRED_NUM_AUDIO_PKTS, GFP_KERNEL);
	if (!urb)
		return -ENOMEM;

	len = usb_endpoint_maxp(port->ep_in);
	buf = usb_alloc_coherent(xone->udev, len * XONE_WIRED_NUM_AUDIO_PKTS,
			GFP_KERNEL, &urb->transfer_dma);
	if (!buf)
		return -ENOMEM;

	urb->dev = xone->udev;
	urb->pipe = usb_rcvisocpipe(xone->udev, port->ep_in->bEndpointAddress);
	urb->transfer_flags = URB_NO_TRANSFER_DMA_MAP;
	urb->transfer_buffer = buf;
	urb->transfer_buffer_length = len * XONE_WIRED_NUM_AUDIO_PKTS;
	urb->number_of_packets = XONE_WIRED_NUM_AUDIO_PKTS;
	urb->interval = port->ep_in->bInterval;
	urb->context = xone;
	urb->complete = xone_wired_audio_in_complete;

	for (i = 0; i < XONE_WIRED_NUM_AUDIO_PKTS; i++) {
		urb->iso_frame_desc[i].offset = i * len;
		urb->iso_frame_desc[i].length = len;
	}

	port->urb_in = urb;
	port->buffer_length_in = len * XONE_WIRED_NUM_AUDIO_PKTS;

	return usb_submit_urb(port->urb_in, GFP_KERNEL);
}

static int xone_wired_init_audio_out(struct gip_adapter *adap, int pkt_len)
{
	struct xone_wired *xone = dev_get_drvdata(&adap->dev);
	struct xone_wired_port *port = &xone->audio_port;
	struct urb *urb;
	void *buf;
	int i, j;

	for (i = 0; i < XONE_WIRED_NUM_AUDIO_URBS; i++) {
		urb = usb_alloc_urb(XONE_WIRED_NUM_AUDIO_PKTS, GFP_KERNEL);
		if (!urb)
			return -ENOMEM;

		buf = usb_alloc_coherent(xone->udev,
				pkt_len * XONE_WIRED_NUM_AUDIO_PKTS,
				GFP_KERNEL, &urb->transfer_dma);
		if (!buf)
			return -ENOMEM;

		urb->dev = xone->udev;
		urb->pipe = usb_sndisocpipe(xone->udev, port->ep_out->bEndpointAddress);
		urb->transfer_flags = URB_NO_TRANSFER_DMA_MAP;
		urb->transfer_buffer = buf;
		urb->transfer_buffer_length = pkt_len * XONE_WIRED_NUM_AUDIO_PKTS;
		urb->number_of_packets = XONE_WIRED_NUM_AUDIO_PKTS;
		urb->interval = port->ep_in->bInterval;
		urb->context = port;
		urb->complete = xone_wired_out_complete;

		for (j = 0; j < XONE_WIRED_NUM_AUDIO_PKTS; j++) {
			urb->iso_frame_desc[j].offset = j * pkt_len;
			urb->iso_frame_desc[j].length = pkt_len;
		}

		usb_anchor_urb(urb, &port->urbs_out_idle);
	}

	port->buffer_length_out = pkt_len * XONE_WIRED_NUM_AUDIO_PKTS;

	return 0;
}

static int xone_wired_disable_audio(struct gip_adapter *adap)
{
	struct xone_wired *xone = dev_get_drvdata(&adap->dev);
	struct xone_wired_port *port = &xone->audio_port;
	int err;

	usb_kill_urb(port->urb_in);
	usb_kill_anchored_urbs(&port->urbs_out_busy);

	err = usb_set_interface(xone->udev, 1, 0);
	xone_wired_free_port(port);

	return err;
}

static struct gip_adapter_ops xone_wired_adapter_ops = {
	.get_buffer = xone_wired_get_buffer,
	.submit_buffer = xone_wired_submit_buffer,
	.enable_audio = xone_wired_enable_audio,
	.init_audio_in = xone_wired_init_audio_in,
	.init_audio_out = xone_wired_init_audio_out,
	.disable_audio = xone_wired_disable_audio,
};

static int xone_wired_probe(struct usb_interface *intf,
		const struct usb_device_id *id)
{
	struct xone_wired *xone;
	int err;

	xone = devm_kzalloc(&intf->dev, sizeof(*xone), GFP_KERNEL);
	if (!xone)
		return -ENOMEM;

	err = usb_find_common_endpoints(intf->cur_altsetting, NULL, NULL,
			&xone->data_port.ep_in, &xone->data_port.ep_out);
	if (err)
		return err;

	xone->udev = interface_to_usbdev(intf);
	xone->data_port.intf = intf;
	init_usb_anchor(&xone->data_port.urbs_out_idle);
	init_usb_anchor(&xone->data_port.urbs_out_busy);
	init_usb_anchor(&xone->audio_port.urbs_out_idle);
	init_usb_anchor(&xone->audio_port.urbs_out_busy);

	/* disable the audio interface */
	/* mandatory for certain third party devices */
	err = usb_set_interface(xone->udev, 1, 0);
	if (err)
		return err;

	err = xone_wired_init_data_in(xone);
	if (err)
		goto err_free_port;

	err = xone_wired_init_data_out(xone);
	if (err)
		goto err_free_port;

	xone->adapter = gip_create_adapter(&intf->dev,
			&xone_wired_adapter_ops, XONE_WIRED_NUM_AUDIO_PKTS);
	if (IS_ERR(xone->adapter)) {
		err = PTR_ERR(xone->adapter);
		goto err_free_port;
	}

	dev_set_drvdata(&xone->adapter->dev, xone);
	usb_set_intfdata(intf, xone);

	err = usb_submit_urb(xone->data_port.urb_in, GFP_KERNEL);
	if (err)
		goto err_destroy_adapter;

	return 0;

err_destroy_adapter:
	gip_destroy_adapter(xone->adapter);
err_free_port:
	xone_wired_free_port(&xone->data_port);

	return err;
}

static void xone_wired_disconnect(struct usb_interface *intf)
{
	struct xone_wired *xone = usb_get_intfdata(intf);
	struct xone_wired_port *data = &xone->data_port;
	struct xone_wired_port *audio = &xone->audio_port;

	usb_kill_urb(data->urb_in);
	usb_kill_urb(audio->urb_in);

	gip_destroy_adapter(xone->adapter);

	usb_kill_anchored_urbs(&data->urbs_out_busy);
	usb_kill_anchored_urbs(&audio->urbs_out_busy);

	xone_wired_free_port(data);
	xone_wired_free_port(audio);

	usb_set_intfdata(intf, NULL);
}

static int xone_wired_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct xone_wired *xone = usb_get_intfdata(intf);
	struct xone_wired_port *data = &xone->data_port;
	struct xone_wired_port *audio = &xone->audio_port;
	int err;

	usb_kill_urb(data->urb_in);
	usb_kill_urb(audio->urb_in);

	/* 1708 controllers disconnect before suspend */
	/* 1537 controllers power on automatically after resume */
	err = gip_suspend_adapter(xone->adapter);
	if (err)
		dev_err(&intf->dev, "%s: suspend adapter failed: %d\n",
				__func__, err);

	if (!usb_wait_anchor_empty_timeout(&data->urbs_out_busy, 1000))
		usb_kill_anchored_urbs(&data->urbs_out_busy);

	if (!usb_wait_anchor_empty_timeout(&audio->urbs_out_busy, 1000))
		usb_kill_anchored_urbs(&audio->urbs_out_busy);

	return err;
}

static int xone_wired_resume(struct usb_interface *intf)
{
	struct xone_wired *xone = usb_get_intfdata(intf);
	int err;

	err = usb_submit_urb(xone->data_port.urb_in, GFP_KERNEL);
	if (err) {
		dev_err(&intf->dev, "%s: submit data failed: %d\n",
					__func__, err);
		return err;
	}

	if (xone->audio_port.urb_in) {
		err = usb_submit_urb(xone->audio_port.urb_in, GFP_KERNEL);
		if (err)
			dev_err(&intf->dev, "%s: submit audio failed: %d\n",
					__func__, err);
	}

	return err;
}

static const struct usb_device_id xone_wired_id_table[] = {
	{ XONE_WIRED_VENDOR(0x045e) }, /* Microsoft */
	{ XONE_WIRED_VENDOR(0x0738) }, /* Mad Catz */
	{ XONE_WIRED_VENDOR(0x0e6f) }, /* PDP */
	{ XONE_WIRED_VENDOR(0x0f0d) }, /* Hori */
	{ XONE_WIRED_VENDOR(0x1532) }, /* Razer */
	{ XONE_WIRED_VENDOR(0x24c6) }, /* PowerA */
	{ XONE_WIRED_VENDOR(0x044f) }, /* Thrustmaster */
	{ XONE_WIRED_VENDOR(0x10f5) }, /* Turtle Beach */
	{ XONE_WIRED_VENDOR(0x2e24) }, /* Hyperkin */
	{ },
};

static struct usb_driver xone_wired_driver = {
	.name = "xone-wired",
	.probe = xone_wired_probe,
	.disconnect = xone_wired_disconnect,
	.suspend = xone_wired_suspend,
	.resume = xone_wired_resume,
	.id_table = xone_wired_id_table,
};

module_usb_driver(xone_wired_driver);

MODULE_DEVICE_TABLE(usb, xone_wired_id_table);
MODULE_AUTHOR("Severin von Wnuck <severinvonw@outlook.de>");
MODULE_DESCRIPTION("xone wired driver");
MODULE_VERSION("#VERSION#");
MODULE_LICENSE("GPL");
