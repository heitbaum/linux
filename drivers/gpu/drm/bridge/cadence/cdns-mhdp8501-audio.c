// SPDX-License-Identifier: GPL-2.0-only
/*
 * Cadence MHDP8501 HDMI audio
 *
 * Copyright (C) Fuzhou Rockchip Electronics Co.Ltd
 * Author: Chris Zhong <zyw@rock-chips.com>
 *
 * Copyright (C) 2019-2024 NXP Semiconductor, Inc.
 */
#include <drm/display/drm_hdmi_state_helper.h>
#include <drm/drm_connector.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <sound/asoundef.h>
#include <sound/hdmi-codec.h>

#include "cdns-mhdp8501-core.h"

/*
 * N and CTS are picked from the TMDS character rate, see the tables in
 * CTA-861 "Audio Sample Clock Capture and Regeneration".
 */
static const u32 tmds_rate_table[] = {
	25200, 27000, 54000, 74250, 148500, 297000, 594000,
};

static const u32 n_table_32k[] = {
	4096, 4096, 4096, 4096, 4096, 3072, 3072,
};

static const u32 n_table_44k[] = {
	6272, 6272, 6272, 6272, 6272, 4704, 9408,
};

static const u32 n_table_48k[] = {
	6144, 6144, 6144, 6144, 6144, 5120, 6144,
};

static int cdns_hdmi_audio_n_index(struct cdns_mhdp8501_device *mhdp, u32 pclk)
{
	int i;

	/*
	 * Zero means no mode is running, so nothing is being transmitted
	 * and the value is never used. Do not warn about that.
	 */
	if (!pclk)
		return 0;

	for (i = 0; i < ARRAY_SIZE(tmds_rate_table); i++)
		if (pclk == tmds_rate_table[i])
			return i;

	dev_warn(mhdp->dev, "pixel clock %u kHz is not in the N table\n", pclk);

	return ARRAY_SIZE(tmds_rate_table) - 1;
}

static void cdns_hdmi_audio_config_i2s(struct cdns_mhdp8501_device *mhdp,
				       struct hdmi_codec_params *hparms,
				       unsigned long long char_rate)
{
	u32 pclk = div_u64(char_rate, 1000);
	int idx = cdns_hdmi_audio_n_index(mhdp, pclk);
	int sub_pckt_num = 4, i2s_port_en_val = 0xf;
	u32 transmission_type = 0;	/* not required for L-PCM */
	u32 audio_type = 0x2;		/* L-PCM */
	int channels = hparms->channels;
	u32 disable_port3 = 0;
	bool non_pcm;
	u32 val, ncts;
	int i;

	non_pcm = hparms->iec.status[0] & IEC958_AES0_NONAUDIO;

	if (channels == 2) {
		i2s_port_en_val = 1;
	} else if (channels == 4) {
		i2s_port_en_val = 3;
	} else if (channels == 6) {
		channels = 8;
		disable_port3 = 1;
	} else if (channels == 8 && non_pcm) {
		audio_type = 0x9;	/* HBR packet type */
		transmission_type = 0x9;
	}

	writel(0, mhdp->regs + SPDIF_CTRL_ADDR);

	val = SYNC_WR_TO_CH_ZERO;
	val |= disable_port3 << 4;
	writel(val, mhdp->regs + FIFO_CNTL);

	val = MAX_NUM_CH(channels);
	val |= NUM_OF_I2S_PORTS(channels);
	val |= audio_type << 7;
	val |= CFG_SUB_PCKT_NUM(sub_pckt_num);
	writel(val, mhdp->regs + SMPL2PKT_CNFG);

	if (hparms->sample_width == 16)
		val = 0;
	else if (hparms->sample_width == 24)
		val = 1 << 9;
	else
		val = 2 << 9;

	val |= AUDIO_CH_NUM(channels);
	val |= I2S_DEC_PORT_EN(i2s_port_en_val);
	val |= TRANS_SMPL_WIDTH_32;
	val |= transmission_type << 13;
	writel(val, mhdp->regs + AUDIO_SRC_CNFG);

	for (i = 0; i < (channels + 1) / 2; i++) {
		if (hparms->sample_width == 16)
			val = (0x02 << 8) | (0x02 << 20);
		else if (hparms->sample_width == 24)
			val = (0x0b << 8) | (0x0b << 20);
		else
			val = 0;

		val |= ((2 * i) << 4) | ((2 * i + 1) << 16);
		writel(val, mhdp->regs + STTS_BIT_CH(i));
	}

	switch (hparms->sample_rate) {
	case 32000:
		val = SAMPLING_FREQ(3) | ORIGINAL_SAMP_FREQ(0xc);
		ncts = n_table_32k[idx];
		break;
	case 44100:
		val = SAMPLING_FREQ(0) | ORIGINAL_SAMP_FREQ(0xf);
		ncts = n_table_44k[idx];
		break;
	case 48000:
		val = SAMPLING_FREQ(2) | ORIGINAL_SAMP_FREQ(0xd);
		ncts = n_table_48k[idx];
		break;
	case 88200:
		val = SAMPLING_FREQ(8) | ORIGINAL_SAMP_FREQ(0x7);
		ncts = n_table_44k[idx] * 2;
		break;
	case 96000:
		val = SAMPLING_FREQ(0xa) | ORIGINAL_SAMP_FREQ(5);
		ncts = n_table_48k[idx] * 2;
		break;
	case 176400:
		val = SAMPLING_FREQ(0xc) | ORIGINAL_SAMP_FREQ(3);
		ncts = n_table_44k[idx] * 4;
		break;
	case 192000:
	default:
		val = SAMPLING_FREQ(0xe) | ORIGINAL_SAMP_FREQ(1);
		ncts = n_table_48k[idx] * 4;
		break;
	}
	val |= 4;
	writel(val, mhdp->regs + COM_CH_STTS_BITS);

	/* N/CTS only mean something while a mode is running */
	if (pclk)
		cdns_mhdp_reg_write(&mhdp->base, CM_I2S_CTRL, ncts | 0x4000000);

	writel(SMPL2PKT_EN, mhdp->regs + SMPL2PKT_CNTL);
	writel(I2S_DEC_START, mhdp->regs + AUDIO_SRC_CNTL);
}

/**
 * cdns_hdmi_audio_prepare() - configure the audio path for a stream
 * @bridge: phandle to the drm bridge.
 * @connector: phandle to the drm connector the stream is played on.
 * @fmt: DAI format negotiated by ASoC.
 * @hparms: stream parameters negotiated by ASoC.
 *
 * Return: 0 on success, a negative error code otherwise.
 */
int cdns_hdmi_audio_prepare(struct drm_bridge *bridge,
			    struct drm_connector *connector,
			    struct hdmi_codec_daifmt *fmt,
			    struct hdmi_codec_params *hparms)
{
	struct cdns_mhdp8501_device *mhdp = bridge->driver_private;
	unsigned long long char_rate = 0;

	if (fmt->fmt != HDMI_I2S) {
		dev_err(mhdp->dev, "invalid format %d, only I2S is supported\n",
			fmt->fmt);
		return -EINVAL;
	}

	/*
	 * N and CTS derive from the TMDS character rate. Read it from the
	 * connector state rather than caching it at modeset, so it stays
	 * correct across a hotplug, and treat no state as no mode.
	 */
	if (connector->state)
		char_rate = connector->state->hdmi.tmds_char_rate;

	/* audio samples are carried in the HDMI stream */
	cdns_mhdp_reg_write(&mhdp->base, CM_CTRL, 8);

	cdns_hdmi_audio_config_i2s(mhdp, hparms, char_rate);

	/*
	 * The sink stays muted until it sees an audio InfoFrame, so this is
	 * not optional. The helper packs it into the connector state and the
	 * bridge connector writes it out through hdmi_write_audio_infoframe().
	 */
	return drm_atomic_helper_connector_hdmi_update_audio_infoframe(connector,
								       &hparms->cea);
}

/**
 * cdns_hdmi_audio_shutdown() - tear the audio path down
 * @bridge: phandle to the drm bridge.
 * @connector: phandle to the drm connector the stream was played on.
 */
void cdns_hdmi_audio_shutdown(struct drm_bridge *bridge,
			      struct drm_connector *connector)
{
	struct cdns_mhdp8501_device *mhdp = bridge->driver_private;

	drm_atomic_helper_connector_hdmi_clear_audio_infoframe(connector);

	writel(0, mhdp->regs + SPDIF_CTRL_ADDR);

	/* clear the audio config and reset the sample source */
	writel(0, mhdp->regs + AUDIO_SRC_CNTL);
	writel(0, mhdp->regs + AUDIO_SRC_CNFG);
	writel(AUDIO_SW_RST, mhdp->regs + AUDIO_SRC_CNTL);
	writel(0, mhdp->regs + AUDIO_SRC_CNTL);

	/* reset the smpl2pckt component */
	writel(0, mhdp->regs + SMPL2PKT_CNTL);
	writel(AUDIO_SW_RST, mhdp->regs + SMPL2PKT_CNTL);
	writel(0, mhdp->regs + SMPL2PKT_CNTL);

	/* reset the FIFO */
	writel(AUDIO_SW_RST, mhdp->regs + FIFO_CNTL);
	writel(0, mhdp->regs + FIFO_CNTL);
}
