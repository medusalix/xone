/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2023 Severin von Wnuck-Lipinski <severinvonw@outlook.de>
 */

#pragma once

#include <linux/types.h>

struct shash_desc *gip_auth_alloc_shash(const char *alg);
int gip_auth_get_transcript(struct shash_desc *desc, void *transcript);
int gip_auth_compute_prf(struct shash_desc *desc, const char *label,
			 u8 *key, int key_len,
			 u8 *seed, int seed_len,
			 u8 *out, int out_len);

int gip_auth_encrypt_rsa(u8 *key, int key_len,
			 u8 *in, int in_len,
			 u8 *out, int out_len);
int gip_auth_compute_ecdh(u8 *pubkey_in, u8 *pubkey_out,
			  int pubkey_len, u8 *secret_hash);
