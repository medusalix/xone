/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2021 Severin von Wnuck-Lipinski <severinvonw@outlook.de>
 */

#pragma once

#include <linux/types.h>
#include <linux/device.h>
#include <linux/semaphore.h>

#include "protocol.h"

#define GIP_MAX_CLIENTS 16

#define gip_register_driver(drv) \
	__gip_register_driver(drv, THIS_MODULE, KBUILD_MODNAME)

#define module_gip_driver(drv) \
	module_driver(drv, gip_register_driver, gip_unregister_driver)

struct gip_adapter_buffer {
	enum gip_adapter_buffer_type {
		GIP_BUF_DATA,
		GIP_BUF_AUDIO,
	} type;

	void *context;
	void *data;
	int length;
};

struct gip_adapter_ops {
	int (*get_buffer)(struct gip_adapter *adap,
			  struct gip_adapter_buffer *buf);
	int (*submit_buffer)(struct gip_adapter *adap,
			     struct gip_adapter_buffer *buf);
	int (*enable_audio)(struct gip_adapter *adap);
	int (*init_audio_in)(struct gip_adapter *adap);
	int (*init_audio_out)(struct gip_adapter *adap, int pkt_len);
	int (*disable_audio)(struct gip_adapter *adap);
};

struct gip_adapter {
	struct device dev;
	int id;

	struct gip_adapter_ops *ops;
	int audio_packet_count;

	struct gip_client *clients[GIP_MAX_CLIENTS];
	struct workqueue_struct *clients_wq;

	/* serializes access to data sequence number */
	spinlock_t send_lock;

	u8 data_sequence;
	u8 audio_sequence;
};

struct gip_client {
	struct device dev;
	u8 id;

	struct gip_adapter *adapter;
	struct gip_driver *drv;
	struct semaphore drv_lock;

	struct work_struct work_register;
	struct work_struct work_unregister;

	struct gip_chunk_buffer *chunk_buf;
	struct gip_hardware hardware;

	struct gip_info_element *external_commands;
	struct gip_info_element *firmware_versions;
	struct gip_info_element *audio_formats;
	struct gip_info_element *capabilities_out;
	struct gip_info_element *capabilities_in;
	struct gip_classes *classes;
	struct gip_info_element *interfaces;
	struct gip_info_element *hid_descriptor;

	struct gip_audio_config audio_config_in;
	struct gip_audio_config audio_config_out;
};

struct gip_driver_ops {
	int (*battery)(struct gip_client *client,
		       enum gip_battery_type type,
		       enum gip_battery_level level);
	int (*guide_button)(struct gip_client *client, bool down);
	int (*audio_ready)(struct gip_client *client);
	int (*audio_volume)(struct gip_client *client, u8 in, u8 out);
	int (*hid_report)(struct gip_client *client, void *data, u32 len);
	int (*input)(struct gip_client *client, void *data, u32 len);
	int (*audio_samples)(struct gip_client *client, void *data, u32 len);
};

struct gip_driver {
	struct device_driver drv;
	const char *name;
	const char *class;

	struct gip_driver_ops ops;

	int (*probe)(struct gip_client *client);
	void (*remove)(struct gip_client *client);
};

struct gip_adapter *gip_create_adapter(struct device *parent,
				       struct gip_adapter_ops *ops,
				       int audio_pkts);
int gip_power_off_adapter(struct gip_adapter *adap);
void gip_destroy_adapter(struct gip_adapter *adap);

struct gip_client *gip_get_client(struct gip_adapter *adap, u8 id);
void gip_add_client(struct gip_client *client);
void gip_remove_client(struct gip_client *client);
void gip_free_client_info(struct gip_client *client);

int __gip_register_driver(struct gip_driver *drv, struct module *owner,
			  const char *mod_name);
void gip_unregister_driver(struct gip_driver *drv);
