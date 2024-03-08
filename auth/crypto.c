// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2023 Severin von Wnuck-Lipinski <severinvonw@outlook.de>
 */

#include <linux/scatterlist.h>
#include <crypto/hash.h>
#include <crypto/sha2.h>
#include <crypto/akcipher.h>
#include <crypto/kpp.h>
#include <crypto/ecdh.h>

#include "crypto.h"

#define GIP_AUTH_ECDH_SECRET_LEN 32

struct shash_desc *gip_auth_alloc_shash(const char *alg)
{
	struct crypto_shash *tfm;
	struct shash_desc *desc;

	tfm = crypto_alloc_shash(alg, 0, 0);
	if (IS_ERR(tfm))
		return ERR_CAST(tfm);

	desc = kzalloc(sizeof(*desc) + crypto_shash_descsize(tfm), GFP_KERNEL);
	if (!desc) {
		crypto_free_shash(tfm);
		return ERR_PTR(-ENOMEM);
	}

	desc->tfm = tfm;
	crypto_shash_init(desc);

	return desc;
}

int gip_auth_get_transcript(struct shash_desc *desc, void *transcript)
{
	struct sha256_state state;
	int err;

	err = crypto_shash_export(desc, &state);
	if (err)
		return err;

	err = crypto_shash_final(desc, transcript);
	if (err)
		return err;

	return crypto_shash_import(desc, &state);
}

int gip_auth_compute_prf(struct shash_desc *desc, const char *label,
			 u8 *key, int key_len,
			 u8 *seed, int seed_len,
			 u8 *out, int out_len)
{
	u8 hash[SHA256_DIGEST_SIZE], hash_out[SHA256_DIGEST_SIZE];
	int err;

	err = crypto_shash_setkey(desc->tfm, key, key_len);
	if (err)
		return err;

	crypto_shash_init(desc);
	crypto_shash_update(desc, label, strlen(label));
	crypto_shash_update(desc, seed, seed_len);
	crypto_shash_final(desc, hash);

	while (out_len > 0) {
		crypto_shash_init(desc);
		crypto_shash_update(desc, hash, sizeof(hash));
		crypto_shash_update(desc, label, strlen(label));
		crypto_shash_update(desc, seed, seed_len);
		crypto_shash_final(desc, hash_out);

		memcpy(out, hash_out, min_t(int, out_len, sizeof(hash)));
		out += sizeof(hash);
		out_len -= sizeof(hash);

		crypto_shash_digest(desc, hash, sizeof(hash), hash);
	}

	return 0;
}

int gip_auth_encrypt_rsa(u8 *key, int key_len,
			 u8 *in, int in_len,
			 u8 *out, int out_len)
{
	struct crypto_akcipher *tfm;
	struct akcipher_request *req;
	struct scatterlist src, dest;
	DECLARE_CRYPTO_WAIT(wait);
	u8 *buf;
	int err;

	buf = kzalloc(out_len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	tfm = crypto_alloc_akcipher("pkcs1pad(rsa,sha256)", 0, 0);
	if (IS_ERR(tfm)) {
		err = PTR_ERR(tfm);
		goto err_free_buf;
	}

	err = crypto_akcipher_set_pub_key(tfm, key, key_len);
	if (err)
		goto err_free_tfm;

	req = akcipher_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		err = -ENOMEM;
		goto err_free_tfm;
	}

	sg_init_one(&src, in, in_len);
	sg_init_one(&dest, buf, out_len);

	akcipher_request_set_crypt(req, &src, &dest, in_len, out_len);
	akcipher_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				      crypto_req_done, &wait);
	err = crypto_wait_req(crypto_akcipher_encrypt(req), &wait);
	if (!err)
		memcpy(out, buf, out_len);

	akcipher_request_free(req);

err_free_tfm:
	crypto_free_akcipher(tfm);
err_free_buf:
	kfree(buf);

	return err;
}

static int gip_auth_ecdh_get_pubkey(struct crypto_kpp *tfm,
				    u8 *out, int len)
{
	struct kpp_request *req;
	struct scatterlist dest;
	struct ecdh key = {};
	DECLARE_CRYPTO_WAIT(wait);
	void *privkey, *pubkey;
	unsigned int privkey_len;
	int err;

	privkey_len = crypto_ecdh_key_len(&key);
	privkey = kzalloc(privkey_len, GFP_KERNEL);
	if (!privkey)
		return -ENOMEM;

	pubkey = kzalloc(len, GFP_KERNEL);
	if (!pubkey)
		goto err_free_privkey;

	/* generate private key */
	err = crypto_ecdh_encode_key(privkey, privkey_len, &key);
	if (err)
		goto err_free_pubkey;

	err = crypto_kpp_set_secret(tfm, privkey, privkey_len);
	if (err)
		goto err_free_pubkey;

	req = kpp_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		err = -ENOMEM;
		goto err_free_pubkey;
	}

	sg_init_one(&dest, pubkey, len);

	kpp_request_set_input(req, NULL, 0);
	kpp_request_set_output(req, &dest, len);
	kpp_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				 crypto_req_done, &wait);
	err = crypto_wait_req(crypto_kpp_generate_public_key(req), &wait);
	if (!err)
		memcpy(out, pubkey, len);

	kpp_request_free(req);

err_free_pubkey:
	kfree(pubkey);
err_free_privkey:
	kfree(privkey);

	return err;
}

static int gip_auth_ecdh_get_secret(struct crypto_kpp *tfm,
				    u8 *pubkey, int pubkey_len,
				    u8 *secret, int secret_len)
{
	struct kpp_request *req;
	struct scatterlist src, dest;
	DECLARE_CRYPTO_WAIT(wait);
	int err;

	req = kpp_request_alloc(tfm, GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	sg_init_one(&src, pubkey, pubkey_len);
	sg_init_one(&dest, secret, secret_len);

	kpp_request_set_input(req, &src, pubkey_len);
	kpp_request_set_output(req, &dest, secret_len);
	kpp_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				 crypto_req_done, &wait);
	err = crypto_wait_req(crypto_kpp_compute_shared_secret(req), &wait);

	kpp_request_free(req);

	return err;
}

int gip_auth_compute_ecdh(u8 *pubkey_in, u8 *pubkey_out,
			  int pubkey_len, u8 *secret_hash)
{
	struct crypto_kpp *tfm_ecdh;
	struct crypto_shash *tfm_sha;
	u8 *secret;
	int err;

	secret = kzalloc(GIP_AUTH_ECDH_SECRET_LEN, GFP_KERNEL);
	if (!secret)
		return -ENOMEM;

	tfm_ecdh = crypto_alloc_kpp("ecdh-nist-p256", 0, 0);
	if (IS_ERR(tfm_ecdh)) {
		err = PTR_ERR(tfm_ecdh);
		goto err_free_secret;
	}

	tfm_sha = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(tfm_sha)) {
		err = PTR_ERR(tfm_sha);
		goto err_free_ecdh;
	}

	err = gip_auth_ecdh_get_pubkey(tfm_ecdh, pubkey_out, pubkey_len);
	if (err)
		goto err_free_sha;

	err = gip_auth_ecdh_get_secret(tfm_ecdh, pubkey_in, pubkey_len,
				       secret, GIP_AUTH_ECDH_SECRET_LEN);
	if (err)
		goto err_free_sha;

	crypto_shash_tfm_digest(tfm_sha, secret, GIP_AUTH_ECDH_SECRET_LEN,
				secret_hash);

err_free_sha:
	crypto_free_shash(tfm_sha);
err_free_ecdh:
	crypto_free_kpp(tfm_ecdh);
err_free_secret:
	kfree(secret);

	return err;
}
