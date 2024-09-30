// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2021 Severin von Wnuck-Lipinski <severinvonw@outlook.de>
 */

#include <linux/module.h>
#include <linux/hrtimer.h>
#include <linux/vmalloc.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>

#include "common.h"
#include "../auth/auth.h"

#define GIP_HS_NAME "Microsoft Xbox Headset"

#define GIP_HS_NUM_BUFFERS 128

/* product ID for the chat headset */
#define GIP_HS_PID_CHAT 0x0111

#define GIP_HS_CONFIG_DELAY msecs_to_jiffies(1000)
#define GIP_HS_POWER_ON_DELAY msecs_to_jiffies(1000)

static const struct snd_pcm_hardware gip_headset_pcm_hw = {
	.info = SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_BATCH |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_CONTINUOUS,
	.periods_min = 2,
	.periods_max = GIP_HS_NUM_BUFFERS,
};

struct gip_headset {
	struct gip_client *client;
	struct gip_battery battery;
	struct gip_auth auth;

	bool chat_headset;

	struct delayed_work work_config;
	struct delayed_work work_power_on;
	struct work_struct work_register;
	bool registered;

	struct hrtimer timer;
	void *buffer;

	struct gip_headset_stream {
		struct snd_pcm_substream *substream;
		snd_pcm_uframes_t pointer;
		snd_pcm_uframes_t period;
	} playback, capture;

	struct snd_card *card;
};

static int gip_headset_pcm_open(struct snd_pcm_substream *sub)
{
	struct gip_headset *headset = snd_pcm_substream_chip(sub);
	struct gip_audio_config *cfg;
	struct snd_pcm_hardware hw = gip_headset_pcm_hw;

	if (sub->stream == SNDRV_PCM_STREAM_PLAYBACK)
		cfg = &headset->client->audio_config_out;
	else
		cfg = &headset->client->audio_config_in;

	hw.rate_min = cfg->sample_rate;
	hw.rate_max = cfg->sample_rate;
	hw.channels_min = cfg->channels;
	hw.channels_max = cfg->channels;
	hw.buffer_bytes_max = cfg->buffer_size * GIP_HS_NUM_BUFFERS;
	hw.period_bytes_min = cfg->buffer_size;
	hw.period_bytes_max = cfg->buffer_size;

	sub->runtime->hw = hw;

	return 0;
}

static int gip_headset_pcm_close(struct snd_pcm_substream *sub)
{
	return 0;
}

static int gip_headset_pcm_hw_params(struct snd_pcm_substream *sub,
				     struct snd_pcm_hw_params *params)
{
	struct snd_pcm_runtime *runtime = sub->runtime;
	size_t size = params_buffer_bytes(params);

	if (runtime->dma_area) {
		if (runtime->dma_bytes >= size)
			return 0; /* Already large enough */
		vfree(runtime->dma_area);
	}
	runtime->dma_area = vzalloc(size);
	if (!runtime->dma_area)
		return -ENOMEM;
	runtime->dma_bytes = size;
	return 1;
}

static int gip_headset_pcm_hw_free(struct snd_pcm_substream *sub)
{
	struct snd_pcm_runtime *runtime = sub->runtime;

	vfree(runtime->dma_area);
	runtime->dma_area = NULL;
	return 0;
}

static struct page *gip_headset_pcm_get_page(struct snd_pcm_substream *sub,
                                             unsigned long offset)
{
	return vmalloc_to_page(sub->runtime->dma_area + offset);
}

static int gip_headset_pcm_prepare(struct snd_pcm_substream *sub)
{
	return 0;
}

static int gip_headset_pcm_trigger(struct snd_pcm_substream *sub, int cmd)
{
	struct gip_headset *headset = snd_pcm_substream_chip(sub);
	struct gip_headset_stream *stream;

	if (sub->stream == SNDRV_PCM_STREAM_PLAYBACK)
		stream = &headset->playback;
	else
		stream = &headset->capture;

	stream->pointer = 0;
	stream->period = 0;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		stream->substream = sub;
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		stream->substream = NULL;
		break;
	default:
		return -EINVAL;
	}

	if (!stream->substream && sub->stream == SNDRV_PCM_STREAM_PLAYBACK)
		memset(headset->buffer, 0,
		       headset->client->audio_config_out.buffer_size);

	return 0;
}

static snd_pcm_uframes_t gip_headset_pcm_pointer(struct snd_pcm_substream *sub)
{
	struct gip_headset *headset = snd_pcm_substream_chip(sub);
	struct gip_headset_stream *stream;

	if (sub->stream == SNDRV_PCM_STREAM_PLAYBACK)
		stream = &headset->playback;
	else
		stream = &headset->capture;

	return bytes_to_frames(sub->runtime, stream->pointer);
}

static const struct snd_pcm_ops gip_headset_pcm_ops = {
	.open = gip_headset_pcm_open,
	.close = gip_headset_pcm_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = gip_headset_pcm_hw_params,
	.hw_free = gip_headset_pcm_hw_free,
	.prepare = gip_headset_pcm_prepare,
	.trigger = gip_headset_pcm_trigger,
	.pointer = gip_headset_pcm_pointer,
	.page = gip_headset_pcm_get_page,
};

static bool gip_headset_advance_pointer(struct gip_headset_stream *stream,
					int len, size_t buf_size)
{
	snd_pcm_uframes_t period = stream->substream->runtime->period_size;

	stream->pointer += len;
	if (stream->pointer >= buf_size)
		stream->pointer -= buf_size;

	stream->period += len;
	if (stream->period >= period) {
		stream->period -= period;
		return true;
	}

	return false;
}

static bool gip_headset_copy_playback(struct gip_headset_stream *stream,
				      unsigned char *data, int len)
{
	unsigned char *src = stream->substream->runtime->dma_area;
	size_t buf_size = snd_pcm_lib_buffer_bytes(stream->substream);
	size_t remaining = buf_size - stream->pointer;

	if (len <= remaining) {
		memcpy(data, src + stream->pointer, len);
	} else {
		memcpy(data, src + stream->pointer, remaining);
		memcpy(data + remaining, src, len - remaining);
	}

	return gip_headset_advance_pointer(stream, len, buf_size);
}

static bool gip_headset_copy_capture(struct gip_headset_stream *stream,
				     unsigned char *data, int len)
{
	unsigned char *dest = stream->substream->runtime->dma_area;
	size_t buf_size = snd_pcm_lib_buffer_bytes(stream->substream);
	size_t remaining = buf_size - stream->pointer;

	if (len <= remaining) {
		memcpy(dest + stream->pointer, data, len);
	} else {
		memcpy(dest + stream->pointer, data, remaining);
		memcpy(dest, data + remaining, len - remaining);
	}

	return gip_headset_advance_pointer(stream, len, buf_size);
}

static enum hrtimer_restart gip_headset_send_samples(struct hrtimer *timer)
{
	struct gip_headset *headset = container_of(timer, typeof(*headset),
						   timer);
	struct gip_audio_config *cfg = &headset->client->audio_config_out;
	struct snd_pcm_substream *sub = headset->playback.substream;
	bool elapsed = false;
	int err;
	unsigned long flags;

	if (sub) {
		snd_pcm_stream_lock_irqsave(sub, flags);

		if (sub->runtime && snd_pcm_running(sub))
			elapsed = gip_headset_copy_playback(&headset->playback,
							    headset->buffer,
							    cfg->buffer_size);

		snd_pcm_stream_unlock_irqrestore(sub, flags);

		if (elapsed)
			snd_pcm_period_elapsed(sub);
	}

	/* retry if driver runs out of buffers */
	err = gip_send_audio_samples(headset->client, headset->buffer);
	if (err && err != -ENOSPC)
		return HRTIMER_NORESTART;

	hrtimer_forward_now(timer, ms_to_ktime(GIP_AUDIO_INTERVAL));

	return HRTIMER_RESTART;
}

static int gip_headset_init_pcm(struct gip_headset *headset)
{
	struct snd_card *card;
	struct snd_pcm *pcm;
	int err;

	err = snd_card_new(&headset->client->dev, SNDRV_DEFAULT_IDX1,
			   SNDRV_DEFAULT_STR1, THIS_MODULE, 0, &card);
	if (err)
		return err;

	strscpy(card->driver, "xone-gip-headset", sizeof(card->driver));
	strscpy(card->shortname, GIP_HS_NAME, sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname), "%s at %s",
		 GIP_HS_NAME, dev_name(&headset->client->dev));

	headset->card = card;

	err = snd_pcm_new(card, GIP_HS_NAME, 0, 1, 1, &pcm);
	if (err)
		return err;

	strscpy(pcm->name, GIP_HS_NAME, sizeof(pcm->name));
	pcm->private_data = headset;

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &gip_headset_pcm_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &gip_headset_pcm_ops);

	return snd_card_register(card);
}

static void gip_headset_config(struct work_struct *work)
{
	struct gip_headset *headset = container_of(to_delayed_work(work),
						   typeof(*headset),
						   work_config);
	struct gip_client *client = headset->client;
	struct gip_info_element *fmts = client->audio_formats;
	int err;

	dev_dbg(&client->dev, "%s: format=0x%02x/0x%02x\n", __func__,
		fmts->data[0], fmts->data[1]);

	/* suggest initial audio format */
	err = gip_suggest_audio_format(client, fmts->data[0], fmts->data[1],
				       headset->chat_headset);
	if (err)
		dev_err(&client->dev, "%s: suggest format failed: %d\n",
			__func__, err);
}

static void gip_headset_power_on(struct work_struct *work)
{
	struct gip_headset *headset = container_of(to_delayed_work(work),
						   typeof(*headset),
						   work_power_on);
	struct gip_client *client = headset->client;
	int err;

	err = gip_set_power_mode(client, GIP_PWR_ON);
	if (err) {
		dev_err(&client->dev, "%s: set power mode failed: %d\n",
			__func__, err);
		return;
	}

	/* not a standalone headset */
	if (client->id)
		return;

	err = gip_init_battery(&headset->battery, client, GIP_HS_NAME);
	if (err) {
		dev_err(&client->dev, "%s: init battery failed: %d\n",
			__func__, err);
		return;
	}

	err = gip_auth_start_handshake(&headset->auth, client);
	if (err)
		dev_err(&client->dev, "%s: start handshake failed: %d\n",
			__func__, err);
}

static void gip_headset_register(struct work_struct *work)
{
	struct gip_headset *headset = container_of(work, typeof(*headset),
						   work_register);
	struct gip_client *client = headset->client;
	int err;

	headset->buffer = devm_kzalloc(&client->dev,
				       client->audio_config_out.buffer_size,
				       GFP_KERNEL);
	if (!headset->buffer)
		return;

	err = gip_headset_init_pcm(headset);
	if (err) {
		dev_err(&client->dev, "%s: init PCM failed: %d\n",
			__func__, err);
		return;
	}

	/* set hardware volume to maximum for headset jack */
	/* standalone & chat headsets have physical volume controls */
	if (client->id && !headset->chat_headset) {
		err = gip_set_audio_volume(client, 100, 50, 100);
		if (err) {
			dev_err(&client->dev, "%s: set volume failed: %d\n",
				__func__, err);
			return;
		}
	}

	err = gip_init_audio_out(client);
	if (err) {
		dev_err(&client->dev, "%s: init audio out failed: %d\n",
			__func__, err);
		return;
	}

	hrtimer_start(&headset->timer, 0, HRTIMER_MODE_REL);
}

static int gip_headset_op_battery(struct gip_client *client,
				  enum gip_battery_type type,
				  enum gip_battery_level level)
{
	struct gip_headset *headset = dev_get_drvdata(&client->dev);

	gip_report_battery(&headset->battery, type, level);

	return 0;
}

static int gip_headset_op_authenticate(struct gip_client *client,
				       void *data, u32 len)
{
	struct gip_headset *headset = dev_get_drvdata(&client->dev);

	return gip_auth_process_pkt(&headset->auth, data, len);
}

static int gip_headset_op_audio_ready(struct gip_client *client)
{
	struct gip_headset *headset = dev_get_drvdata(&client->dev);

	schedule_delayed_work(&headset->work_power_on, GIP_HS_POWER_ON_DELAY);

	return 0;
}

static int gip_headset_op_audio_volume(struct gip_client *client,
				       u8 in, u8 out)
{
	struct gip_headset *headset = dev_get_drvdata(&client->dev);

	/* headset reported initial volume, start audio I/O */
	if (!headset->registered) {
		schedule_work(&headset->work_register);
		headset->registered = true;
	}

	/* ignore hardware volume, let software handle volume changes */
	return 0;
}

static int gip_headset_op_audio_samples(struct gip_client *client,
					void *data, u32 len)
{
	struct gip_headset *headset = dev_get_drvdata(&client->dev);
	struct snd_pcm_substream *sub = headset->capture.substream;
	bool elapsed = false;
	unsigned long flags;

	if (!sub)
		return 0;

	snd_pcm_stream_lock_irqsave(sub, flags);

	if (sub->runtime && snd_pcm_running(sub))
		elapsed = gip_headset_copy_capture(&headset->capture,
						   data, len);

	snd_pcm_stream_unlock_irqrestore(sub, flags);

	if (elapsed)
		snd_pcm_period_elapsed(sub);

	return 0;
}

static int gip_headset_probe(struct gip_client *client)
{
	struct gip_headset *headset;
	struct gip_info_element *fmts = client->audio_formats;
	int err;

	if (!fmts || !fmts->count)
		return -ENODEV;

	headset = devm_kzalloc(&client->dev, sizeof(*headset), GFP_KERNEL);
	if (!headset)
		return -ENOMEM;

	headset->client = client;
	headset->chat_headset = client->hardware.vendor == GIP_VID_MICROSOFT &&
				client->hardware.product == GIP_HS_PID_CHAT;

	INIT_DELAYED_WORK(&headset->work_config, gip_headset_config);
	INIT_DELAYED_WORK(&headset->work_power_on, gip_headset_power_on);
	INIT_WORK(&headset->work_register, gip_headset_register);

	hrtimer_init(&headset->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	headset->timer.function = gip_headset_send_samples;

	err = gip_enable_audio(client);
	if (err)
		return err;

	err = gip_init_audio_in(client);
	if (err) {
		gip_disable_audio(client);
		return err;
	}

	dev_set_drvdata(&client->dev, headset);

	/* delay to prevent response from being dropped */
	schedule_delayed_work(&headset->work_config, GIP_HS_CONFIG_DELAY);

	return 0;
}

static void gip_headset_remove(struct gip_client *client)
{
	struct gip_headset *headset = dev_get_drvdata(&client->dev);

	cancel_delayed_work_sync(&headset->work_config);
	cancel_delayed_work_sync(&headset->work_power_on);
	cancel_work_sync(&headset->work_register);
	hrtimer_cancel(&headset->timer);
	gip_disable_audio(client);

	if (headset->card) {
		snd_card_disconnect(headset->card);
		snd_card_free_when_closed(headset->card);
	}
}

static struct gip_driver gip_headset_driver = {
	.name = "xone-gip-headset",
	.class = "Windows.Xbox.Input.Headset",
	.ops = {
		.battery = gip_headset_op_battery,
		.authenticate = gip_headset_op_authenticate,
		.audio_ready = gip_headset_op_audio_ready,
		.audio_volume = gip_headset_op_audio_volume,
		.audio_samples = gip_headset_op_audio_samples,
	},
	.probe = gip_headset_probe,
	.remove = gip_headset_remove,
};
module_gip_driver(gip_headset_driver);

MODULE_ALIAS("gip:Windows.Xbox.Input.Headset");
MODULE_AUTHOR("Severin von Wnuck-Lipinski <severinvonw@outlook.de>");
MODULE_DESCRIPTION("xone GIP headset driver");
MODULE_VERSION("#VERSION#");
MODULE_LICENSE("GPL");
