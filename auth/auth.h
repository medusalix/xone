/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2023 Severin von Wnuck-Lipinski <severinvonw@outlook.de>
 */

#pragma once

#include <linux/types.h>
#include <linux/workqueue.h>

/* trailer is required for v1 clients */
#define GIP_AUTH_TRAILER_LEN 8
#define GIP_AUTH_RANDOM_LEN 32
#define GIP_AUTH_CERTIFICATE_MAX_LEN 1024
#define GIP_AUTH_PUBKEY_LEN 270
#define GIP_AUTH_SECRET_LEN 48
#define GIP_AUTH_ENCRYPTED_PMS_LEN 256
#define GIP_AUTH_TRANSCRIPT_LEN 32
#define GIP_AUTH_SESSION_KEY_LEN 16

#define GIP_AUTH2_PUBKEY_LEN 64
#define GIP_AUTH2_SECRET_LEN 32

struct gip_client;

struct gip_auth {
	struct gip_client *client;

	struct shash_desc *shash_transcript;
	struct shash_desc *shash_prf;

	struct work_struct work_exchange_rsa;
	struct work_struct work_exchange_ecdh;
	struct work_struct work_complete;

	u8 last_sent_command;

	u8 random_host[GIP_AUTH_RANDOM_LEN];
	u8 random_client[GIP_AUTH_RANDOM_LEN];

	u8 pubkey_client[GIP_AUTH_PUBKEY_LEN];
	u8 pubkey_client2[GIP_AUTH2_PUBKEY_LEN];

	u8 master_secret[GIP_AUTH_SECRET_LEN];
};

int gip_auth_process_pkt(struct gip_auth *auth, void *data, u32 len);
int gip_auth_start_handshake(struct gip_auth *auth, struct gip_client *client);
