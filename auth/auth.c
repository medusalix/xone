// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2023 Severin von Wnuck-Lipinski <severinvonw@outlook.de>
 */

#include <linux/random.h>
#include <crypto/hash.h>

#include "auth.h"
#include "crypto.h"
#include "../bus/bus.h"

enum gip_auth_context {
	GIP_AUTH_CTX_HANDSHAKE = 0x00,
	GIP_AUTH_CTX_CONTROL = 0x01,
};

enum gip_auth_command_handshake {
	GIP_AUTH_CMD_HOST_HELLO = 0x01,
	GIP_AUTH_CMD_CLIENT_HELLO = 0x02,
	GIP_AUTH_CMD_CLIENT_CERTIFICATE = 0x03,
	GIP_AUTH_CMD_HOST_SECRET = 0x05,
	GIP_AUTH_CMD_HOST_FINISH = 0x07,
	GIP_AUTH_CMD_CLIENT_FINISH = 0x08,

	GIP_AUTH2_CMD_HOST_HELLO = 0x21,
	GIP_AUTH2_CMD_CLIENT_HELLO = 0x22,
	GIP_AUTH2_CMD_CLIENT_CERTIFICATE = 0x23,
	GIP_AUTH2_CMD_CLIENT_PUBKEY = 0x24,
	GIP_AUTH2_CMD_HOST_PUBKEY = 0x25,
	GIP_AUTH2_CMD_HOST_FINISH = 0x26,
	GIP_AUTH2_CMD_CLIENT_FINISH = 0x27,
};

enum gip_auth_command_control {
	GIP_AUTH_CTRL_COMPLETE = 0x00,
	GIP_AUTH_CTRL_RESET = 0x01,
};

enum gip_auth_option {
	GIP_AUTH_OPT_ACKNOWLEDGE = BIT(0),
	GIP_AUTH_OPT_REQUEST = BIT(1),
	GIP_AUTH_OPT_FROM_HOST = BIT(6),
	GIP_AUTH_OPT_FROM_CLIENT = BIT(6) | BIT(7),
};

struct gip_auth_header_handshake {
	u8 context;
	u8 options;
	u8 error;
	u8 command;
	__be16 length;
} __packed;

struct gip_auth_header_data {
	u8 command;
	u8 version;
	__be16 length;
} __packed;

struct gip_auth_header_full {
	struct gip_auth_header_handshake handshake;
	struct gip_auth_header_data data;
} __packed;

struct gip_auth_header_control {
	u8 context;
	u8 control;
} __packed;

struct gip_auth_request {
	struct gip_auth_header_handshake header;

	u8 trailer[GIP_AUTH_TRAILER_LEN];
} __packed;

struct gip_auth_pkt_host_hello {
	struct gip_auth_header_full header;

	u8 random[GIP_AUTH_RANDOM_LEN];
	u8 unknown1[4];
	u8 unknown2[4];

	u8 trailer[GIP_AUTH_TRAILER_LEN];
} __packed;

struct gip_auth_pkt_host_secret {
	struct gip_auth_header_full header;

	u8 encrypted_pms[GIP_AUTH_ENCRYPTED_PMS_LEN];

	u8 trailer[GIP_AUTH_TRAILER_LEN];
} __packed;

struct gip_auth_pkt_host_finish {
	struct gip_auth_header_full header;

	u8 transcript[GIP_AUTH_TRANSCRIPT_LEN];

	u8 trailer[GIP_AUTH_TRAILER_LEN];
} __packed;

struct gip_auth_pkt_client_hello {
	u8 random[GIP_AUTH_RANDOM_LEN];
	u8 unknown[48];
} __packed;

struct gip_auth_pkt_client_finish {
	u8 transcript[GIP_AUTH_TRANSCRIPT_LEN];
	u8 unknown[32];
} __packed;

struct gip_auth2_pkt_host_hello {
	struct gip_auth_header_full header;

	u8 random[GIP_AUTH_RANDOM_LEN];
	u8 unknown[4];

	u8 trailer[GIP_AUTH_TRAILER_LEN];
} __packed;

struct gip_auth2_pkt_host_pubkey {
	struct gip_auth_header_full header;

	u8 pubkey[GIP_AUTH2_PUBKEY_LEN];

	u8 trailer[GIP_AUTH_TRAILER_LEN];
} __packed;

struct gip_auth2_pkt_host_finish {
	struct gip_auth_header_full header;

	u8 transcript[GIP_AUTH_TRANSCRIPT_LEN];

	u8 trailer[GIP_AUTH_TRAILER_LEN];
} __packed;

struct gip_auth2_pkt_client_hello {
	u8 random[GIP_AUTH_RANDOM_LEN];
	u8 unknown1[108];
	u8 unknown2[32];
} __packed;

struct gip_auth2_pkt_client_cert {
	char header[4];
	u8 unknown1[136];
	char chip[32];
	char revision[20];
	u8 unknown2[576];
} __packed;

struct gip_auth2_pkt_client_pubkey {
	u8 pubkey[GIP_AUTH2_PUBKEY_LEN];
	u8 unknown[64];
} __packed;

struct gip_auth2_pkt_client_finish {
	u8 transcript[GIP_AUTH_TRANSCRIPT_LEN];
	u8 unknown[32];
} __packed;

static int gip_auth_send_pkt(struct gip_auth *auth,
			     enum gip_auth_command_handshake cmd,
			     void *pkt, u16 len)
{
	struct gip_auth_header_full *hdr = pkt;
	u16 data_len = len - sizeof(hdr->handshake) - GIP_AUTH_TRAILER_LEN;

	hdr->handshake.context = GIP_AUTH_CTX_HANDSHAKE;
	hdr->handshake.options = GIP_AUTH_OPT_ACKNOWLEDGE |
				 GIP_AUTH_OPT_FROM_HOST;
	hdr->handshake.command = cmd;
	hdr->handshake.length = cpu_to_be16(data_len);

	hdr->data.command = cmd;
	hdr->data.version = cmd >= GIP_AUTH2_CMD_HOST_HELLO ? 0x02 : 0x01;
	hdr->data.length = cpu_to_be16(data_len - sizeof(hdr->data));

	auth->last_sent_command = cmd;
	crypto_shash_update(auth->shash_transcript,
			    pkt + sizeof(hdr->handshake), data_len);

	return gip_send_authenticate(auth->client, pkt, len, true);
}

static int gip_auth_request_pkt(struct gip_auth *auth,
				enum gip_auth_command_handshake cmd, u16 len)
{
	struct gip_auth_request req = {};
	u16 data_len = len + sizeof(struct gip_auth_header_data);

	req.header.context = GIP_AUTH_CTX_HANDSHAKE;
	req.header.options = GIP_AUTH_OPT_REQUEST | GIP_AUTH_OPT_FROM_HOST;
	req.header.command = cmd;
	req.header.length = cpu_to_be16(data_len);

	return gip_send_authenticate(auth->client, &req, sizeof(req), true);
}

static int gip_auth2_send_hello(struct gip_auth *auth)
{
	struct gip_auth2_pkt_host_hello pkt = {};

	/* reset transcript after protocol upgrade */
	crypto_shash_init(auth->shash_transcript);

	get_random_bytes(auth->random_host, sizeof(auth->random_host));
	memcpy(pkt.random, auth->random_host, sizeof(pkt.random));

	return gip_auth_send_pkt(auth, GIP_AUTH2_CMD_HOST_HELLO,
				 &pkt, sizeof(pkt));
}

static int gip_auth2_handle_pkt_hello(struct gip_auth *auth,
				      void *data, u32 len)
{
	struct gip_auth2_pkt_client_hello *pkt = data;

	if (len < sizeof(*pkt))
		return -EINVAL;

	memcpy(auth->random_client, pkt->random, sizeof(auth->random_client));

	return gip_auth_request_pkt(auth, GIP_AUTH2_CMD_CLIENT_CERTIFICATE,
				    sizeof(struct gip_auth2_pkt_client_cert));
}

static int gip_auth2_handle_pkt_certificate(struct gip_auth *auth,
					    void *data, u32 len)
{
	struct gip_auth2_pkt_client_cert *pkt = data;

	if (len < sizeof(*pkt))
		return -EINVAL;

	dev_dbg(&auth->client->dev,
		"%s: header=%.*s, chip=%.*s, revision=%.*s\n", __func__,
		(int)sizeof(pkt->header), pkt->header,
		(int)sizeof(pkt->chip), pkt->chip,
		(int)sizeof(pkt->revision), pkt->revision);

	return gip_auth_request_pkt(auth, GIP_AUTH2_CMD_CLIENT_PUBKEY,
				    sizeof(struct gip_auth2_pkt_client_pubkey));
}

static int gip_auth2_handle_pkt_pubkey(struct gip_auth *auth,
				       void *data, u32 len)
{
	struct gip_auth2_pkt_client_pubkey *pkt = data;

	if (len < sizeof(*pkt))
		return -EINVAL;

	memcpy(auth->pubkey_client2, pkt->pubkey, sizeof(pkt->pubkey));
	schedule_work(&auth->work_exchange_ecdh);

	return 0;
}

static int gip_auth2_compute_master_secret(struct gip_auth *auth,
					   u8 *pubkey, int len)
{
	u8 random[GIP_AUTH_RANDOM_LEN * 2];
	u8 secret[GIP_AUTH2_SECRET_LEN];
	int err;

	memcpy(random, auth->random_host, sizeof(auth->random_host));
	memcpy(random + sizeof(auth->random_host), auth->random_client,
	       sizeof(auth->random_client));

	err = gip_auth_compute_ecdh(auth->pubkey_client2, pubkey,
				    len, secret);
	if (err)
		return err;

	return gip_auth_compute_prf(auth->shash_prf, "Master Secret",
				    secret, sizeof(secret),
				    random, sizeof(random),
				    auth->master_secret,
				    sizeof(auth->master_secret));
}

static void gip_auth2_exchange_ecdh(struct work_struct *work)
{
	struct gip_auth *auth = container_of(work, typeof(*auth),
					     work_exchange_ecdh);
	struct gip_auth2_pkt_host_pubkey *pkt;
	int err;

	pkt = kzalloc(sizeof(*pkt), GFP_KERNEL);
	if (!pkt)
		return;

	err = gip_auth2_compute_master_secret(auth, pkt->pubkey,
					      sizeof(pkt->pubkey));
	if (err) {
		dev_err(&auth->client->dev, "%s: compute secret failed\n",
			__func__);
		goto err_free_pkt;
	}

	err = gip_auth_send_pkt(auth, GIP_AUTH2_CMD_HOST_PUBKEY,
				pkt, sizeof(*pkt));
	if (err)
		dev_err(&auth->client->dev, "%s: send pkt failed: %d\n",
			__func__, err);

err_free_pkt:
	kfree(pkt);
}

int gip_auth_send_pkt_hello(struct gip_auth *auth)
{
	struct gip_auth_pkt_host_hello pkt = {};

	get_random_bytes(auth->random_host, sizeof(auth->random_host));
	memcpy(pkt.random, auth->random_host, sizeof(pkt.random));

	return gip_auth_send_pkt(auth, GIP_AUTH_CMD_HOST_HELLO,
				 &pkt, sizeof(pkt));
}

static int gip_auth_send_pkt_finish(struct gip_auth *auth,
				    enum gip_auth_command_handshake cmd)
{
	struct gip_auth_pkt_host_finish pkt = {};
	u8 transcript[GIP_AUTH_TRANSCRIPT_LEN];
	int err;

	err = gip_auth_get_transcript(auth->shash_transcript, transcript);
	if (err) {
		dev_err(&auth->client->dev, "%s: get transcript failed: %d\n",
			__func__, err);
		return err;
	}

	err = gip_auth_compute_prf(auth->shash_prf, "Host Finished",
				   auth->master_secret,
				   sizeof(auth->master_secret),
				   transcript, sizeof(transcript),
				   pkt.transcript, sizeof(pkt.transcript));
	if (err) {
		dev_err(&auth->client->dev, "%s: compute PRF failed: %d\n",
			__func__, err);
		return err;
	}

	return gip_auth_send_pkt(auth, cmd, &pkt, sizeof(pkt));
}

static int gip_auth_handle_pkt_acknowledge(struct gip_auth *auth)
{
	switch (auth->last_sent_command) {
	case GIP_AUTH2_CMD_HOST_HELLO:
		return gip_auth_request_pkt(auth, GIP_AUTH2_CMD_CLIENT_HELLO,
					    sizeof(struct gip_auth2_pkt_client_hello));
	case GIP_AUTH2_CMD_HOST_PUBKEY:
		return gip_auth_send_pkt_finish(auth, GIP_AUTH2_CMD_HOST_FINISH);
	case GIP_AUTH2_CMD_HOST_FINISH:
		return gip_auth_request_pkt(auth, GIP_AUTH2_CMD_CLIENT_FINISH,
					    sizeof(struct gip_auth2_pkt_client_finish));
	case GIP_AUTH_CMD_HOST_HELLO:
		return gip_auth_request_pkt(auth, GIP_AUTH_CMD_CLIENT_HELLO,
					    sizeof(struct gip_auth_pkt_client_hello));
	case GIP_AUTH_CMD_HOST_SECRET:
		return gip_auth_send_pkt_finish(auth, GIP_AUTH_CMD_HOST_FINISH);
	case GIP_AUTH_CMD_HOST_FINISH:
		return gip_auth_request_pkt(auth, GIP_AUTH_CMD_CLIENT_FINISH,
					    sizeof(struct gip_auth_pkt_client_finish));
	default:
		return -EPROTO;
	}
}

static int gip_auth_handle_pkt_hello(struct gip_auth *auth,
				     void *data, u32 len)
{
	struct gip_auth_pkt_client_hello *pkt = data;

	if (len < sizeof(*pkt))
		return -EINVAL;

	memcpy(auth->random_client, pkt->random, sizeof(pkt->random));

	return gip_auth_request_pkt(auth, GIP_AUTH_CMD_CLIENT_CERTIFICATE,
				    GIP_AUTH_CERTIFICATE_MAX_LEN);
}

static int gip_auth_handle_pkt_certificate(struct gip_auth *auth,
					   void *data, u32 len)
{
	/* ASN.1 SEQUENCE (len = 0x04 + 0x010a) */
	u8 asn1_seq[] = { 0x30, 0x82, 0x01, 0x0a };
	int i;

	if (len > GIP_AUTH_CERTIFICATE_MAX_LEN)
		return -EINVAL;

	/* poor way of extracting a pubkey from an X.509 certificate */
	/* the certificates issued by Microsoft do not comply with RFC 5280 */
	/* they have an empty subject and no subjectAltName */
	/* this is explicitly forbidden by section 4.2.1.6 of the RFC */
	/* the kernel's ASN.1 parser will fail when using x509_cert_parse */
	for (i = 0; i + sizeof(asn1_seq) <= len; i++) {
		if (memcmp(data + i, asn1_seq, sizeof(asn1_seq)))
			continue;

		if (i + GIP_AUTH_PUBKEY_LEN > len)
			return -EINVAL;

		memcpy(auth->pubkey_client, data + i, GIP_AUTH_PUBKEY_LEN);
		schedule_work(&auth->work_exchange_rsa);

		return 0;
	}

	return -EPROTO;
}

static int gip_auth_handle_pkt_finish(struct gip_auth *auth,
				      void *data, u32 len)
{
	struct gip_auth_pkt_client_finish *pkt = data;
	u8 transcript[GIP_AUTH_TRANSCRIPT_LEN];
	u8 finished[GIP_AUTH_TRANSCRIPT_LEN];
	int err;

	if (len < sizeof(*pkt))
		return -EINVAL;

	err = gip_auth_get_transcript(auth->shash_transcript, transcript);
	if (err) {
		dev_err(&auth->client->dev, "%s: get transcript failed: %d\n",
			__func__, err);
		return err;
	}

	err = gip_auth_compute_prf(auth->shash_prf, "Device Finished",
				   auth->master_secret,
				   sizeof(auth->master_secret),
				   transcript, sizeof(transcript),
				   finished, sizeof(finished));
	if (err) {
		dev_err(&auth->client->dev, "%s: compute PRF failed: %d\n",
			__func__, err);
		return err;
	}

	if (memcmp(pkt->transcript, finished, sizeof(finished))) {
		dev_err(&auth->client->dev, "%s: transcript mismatch\n",
			__func__);
		return -EPROTO;
	}

	schedule_work(&auth->work_complete);

	return 0;
}

static int gip_auth_compute_master_secret(struct gip_auth *auth,
					  u8 *encrypted_pms, int len)
{
	u8 random[GIP_AUTH_RANDOM_LEN * 2];
	u8 *pms;
	int err;

	memcpy(random, auth->random_host, sizeof(auth->random_host));
	memcpy(random + sizeof(auth->random_host), auth->random_client,
	       sizeof(auth->random_client));

	pms = kmalloc(GIP_AUTH_SECRET_LEN, GFP_KERNEL);
	if (!pms)
		return -ENOMEM;

	/* get random premaster secret */
	get_random_bytes(pms, sizeof(pms));

	err = gip_auth_encrypt_rsa(auth->pubkey_client, GIP_AUTH_PUBKEY_LEN,
				   pms, GIP_AUTH_SECRET_LEN, encrypted_pms,
				   len);
	if (err)
		goto err_free_pms;

	err = gip_auth_compute_prf(auth->shash_prf, "Master Secret",
				   pms, sizeof(pms), random, sizeof(random),
				   auth->master_secret,
				   sizeof(auth->master_secret));

err_free_pms:
	kfree(pms);

	return err;
}

static void gip_auth_exchange_rsa(struct work_struct *work)
{
	struct gip_auth *auth = container_of(work, typeof(*auth),
					     work_exchange_rsa);
	struct gip_auth_pkt_host_secret *pkt;
	int err;

	pkt = kzalloc(sizeof(*pkt), GFP_KERNEL);
	if (!pkt)
		return;

	err = gip_auth_compute_master_secret(auth, pkt->encrypted_pms,
					     sizeof(pkt->encrypted_pms));
	if (err) {
		dev_err(&auth->client->dev, "%s: compute secret failed\n",
			__func__);
		goto err_free_pkt;
	}

	err = gip_auth_send_pkt(auth, GIP_AUTH_CMD_HOST_SECRET,
				pkt, sizeof(*pkt));
	if (err)
		dev_err(&auth->client->dev, "%s: send pkt failed: %d\n",
			__func__, err);

err_free_pkt:
	kfree(pkt);
}

static void gip_auth_complete_handshake(struct work_struct *work)
{
	struct gip_auth *auth = container_of(work, typeof(*auth),
					     work_complete);
	struct gip_auth_header_control hdr = {};
	u8 random[GIP_AUTH_RANDOM_LEN * 2];
	u8 key[GIP_AUTH_SESSION_KEY_LEN];
	int err;

	hdr.context = GIP_AUTH_CTX_CONTROL;
	hdr.control = GIP_AUTH_CTRL_COMPLETE;

	memcpy(random, auth->random_host, sizeof(auth->random_host));
	memcpy(random + sizeof(auth->random_host), auth->random_client,
	       sizeof(auth->random_client));

	err = gip_auth_compute_prf(auth->shash_prf,
				   "EXPORTER DAWN data channel session key for controller",
				   auth->master_secret,
				   sizeof(auth->master_secret),
				   random, sizeof(random),
				   key, sizeof(key));
	if (err) {
		dev_err(&auth->client->dev, "%s: compute PRF failed: %d\n",
			__func__, err);
		return;
	}

	dev_dbg(&auth->client->dev, "%s: key=%*phD\n", __func__,
		(int)sizeof(key), key);

	err = gip_send_authenticate(auth->client, &hdr, sizeof(hdr), false);
	if (err) {
		dev_err(&auth->client->dev, "%s: send pkt failed: %d\n",
			__func__, err);
		return;
	}

	err = gip_set_encryption_key(auth->client, key, sizeof(key));
	if (err)
		dev_err(&auth->client->dev,
			"%s: set encryption key failed: %d\n", __func__, err);
}

int gip_auth_dispatch_pkt(struct gip_auth *auth,
			  enum gip_auth_command_handshake cmd,
			  void *data, u32 len)
{
	switch (cmd) {
	case GIP_AUTH2_CMD_CLIENT_HELLO:
		return gip_auth2_handle_pkt_hello(auth, data, len);
	case GIP_AUTH2_CMD_CLIENT_CERTIFICATE:
		return gip_auth2_handle_pkt_certificate(auth, data, len);
	case GIP_AUTH2_CMD_CLIENT_PUBKEY:
		return gip_auth2_handle_pkt_pubkey(auth, data, len);
	case GIP_AUTH2_CMD_CLIENT_FINISH:
		return gip_auth_handle_pkt_finish(auth, data, len);
	case GIP_AUTH_CMD_CLIENT_HELLO:
		return gip_auth_handle_pkt_hello(auth, data, len);
	case GIP_AUTH_CMD_CLIENT_CERTIFICATE:
		return gip_auth_handle_pkt_certificate(auth, data, len);
	case GIP_AUTH_CMD_CLIENT_FINISH:
		return gip_auth_handle_pkt_finish(auth, data, len);
	default:
		return -EPROTO;
	}
}

static int gip_auth_process_pkt_data(struct gip_auth *auth, void *data, u32 len)
{
	struct gip_auth_header_full *hdr = data;
	int err;

	if (len < sizeof(*hdr))
		return -EINVAL;

	/* client uses auth v2 */
	if (hdr->handshake.command != hdr->data.command) {
		dev_dbg(&auth->client->dev, "%s: protocol upgrade\n", __func__);
		return gip_auth2_send_hello(auth);
	}

	err = gip_auth_dispatch_pkt(auth, hdr->data.command,
				    data + sizeof(*hdr), len - sizeof(*hdr));
	if (err)
		return err;

	return crypto_shash_update(auth->shash_transcript,
				   data + sizeof(hdr->handshake),
				   len - sizeof(hdr->handshake));
}

int gip_auth_process_pkt(struct gip_auth *auth, void *data, u32 len)
{
	struct gip_auth_header_handshake *hdr = data;

	if (!auth->client)
		return -ENODEV;

	if (len < sizeof(*hdr))
		return -EINVAL;

	if (hdr->error)
		return -EPROTO;

	if (hdr->options & GIP_AUTH_OPT_ACKNOWLEDGE)
		return gip_auth_handle_pkt_acknowledge(auth);

	return gip_auth_process_pkt_data(auth, data, len);
}
EXPORT_SYMBOL_GPL(gip_auth_process_pkt);

static void gip_auth_release(void *res)
{
	struct gip_auth *auth = res;

	cancel_work_sync(&auth->work_exchange_rsa);
	cancel_work_sync(&auth->work_exchange_ecdh);
	cancel_work_sync(&auth->work_complete);

	crypto_free_shash(auth->shash_transcript->tfm);
	crypto_free_shash(auth->shash_prf->tfm);
	kfree(auth->shash_transcript);
	kfree(auth->shash_prf);

	auth->client = NULL;
	auth->shash_transcript = NULL;
	auth->shash_prf = NULL;
}

int gip_auth_start_handshake(struct gip_auth *auth, struct gip_client *client)
{
	struct shash_desc *shash_transcript, *shash_prf;
	int err;

	shash_transcript = gip_auth_alloc_shash("sha256");
	if (IS_ERR(shash_transcript))
		return PTR_ERR(shash_transcript);

	shash_prf = gip_auth_alloc_shash("hmac(sha256)");
	if (IS_ERR(shash_prf)) {
		crypto_free_shash(shash_transcript->tfm);
		kfree(shash_transcript);
		return PTR_ERR(shash_prf);
	}

	auth->client = client;
	auth->shash_transcript = shash_transcript;
	auth->shash_prf = shash_prf;

	INIT_WORK(&auth->work_exchange_rsa, gip_auth_exchange_rsa);
	INIT_WORK(&auth->work_exchange_ecdh, gip_auth2_exchange_ecdh);
	INIT_WORK(&auth->work_complete, gip_auth_complete_handshake);

	err = devm_add_action_or_reset(&client->dev, gip_auth_release, auth);
	if (err)
		return err;

	return gip_auth_send_pkt_hello(auth);
}
EXPORT_SYMBOL_GPL(gip_auth_start_handshake);
