// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright(c) 2023 Intel Corporation. All rights reserved.

#include <sound/soc.h>
#include "../common/soc-intel-quirks.h"
#include "hda_dsp_common.h"
#include "sof_board_helpers.h"

/*
 * Intel HDMI DAI Link
 */
static int hdmi_init(struct snd_soc_pcm_runtime *rtd)
{
	struct sof_card_private *ctx = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *dai = snd_soc_rtd_to_codec(rtd, 0);

	ctx->hdmi.hdmi_comp = dai->component;

	return 0;
}

int sof_intel_board_card_late_probe(struct snd_soc_card *card)
{
	struct sof_card_private *ctx = snd_soc_card_get_drvdata(card);

	if (!ctx->hdmi_num)
		return 0;

	if (!ctx->hdmi.idisp_codec)
		return 0;

	if (!ctx->hdmi.hdmi_comp)
		return -EINVAL;

	return hda_dsp_hdmi_build_controls(card, ctx->hdmi.hdmi_comp);
}
EXPORT_SYMBOL_NS(sof_intel_board_card_late_probe, SND_SOC_INTEL_SOF_BOARD_HELPERS);

/*
 * DMIC DAI Link
 */
static const struct snd_soc_dapm_widget dmic_widgets[] = {
	SND_SOC_DAPM_MIC("SoC DMIC", NULL),
};

static const struct snd_soc_dapm_route dmic_routes[] = {
	{"DMic", NULL, "SoC DMIC"},
};

static int dmic_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_card *card = rtd->card;
	int ret;

	ret = snd_soc_dapm_new_controls(&card->dapm, dmic_widgets,
					ARRAY_SIZE(dmic_widgets));
	if (ret) {
		dev_err(rtd->dev, "fail to add dmic widgets, ret %d\n", ret);
		return ret;
	}

	ret = snd_soc_dapm_add_routes(&card->dapm, dmic_routes,
				      ARRAY_SIZE(dmic_routes));
	if (ret) {
		dev_err(rtd->dev, "fail to add dmic routes, ret %d\n", ret);
		return ret;
	}

	return 0;
}

/*
 * DAI Link Helpers
 */
static struct snd_soc_dai_link_component dmic_component[] = {
	{
		.name = "dmic-codec",
		.dai_name = "dmic-hifi",
	}
};

static struct snd_soc_dai_link_component platform_component[] = {
	{
		/* name might be overridden during probe */
		.name = "0000:00:1f.3"
	}
};

int sof_intel_board_set_codec_link(struct device *dev,
				   struct snd_soc_dai_link *link, int be_id,
				   enum sof_ssp_codec codec_type, int ssp_codec)
{
	struct snd_soc_dai_link_component *cpus;

	dev_dbg(dev, "link %d: codec %s, ssp %d\n", be_id,
		sof_ssp_get_codec_name(codec_type), ssp_codec);

	/* link name */
	link->name = devm_kasprintf(dev, GFP_KERNEL, "SSP%d-Codec", ssp_codec);
	if (!link->name)
		return -ENOMEM;

	/* cpus */
	cpus = devm_kzalloc(dev, sizeof(struct snd_soc_dai_link_component),
			    GFP_KERNEL);
	if (!cpus)
		return -ENOMEM;

	if (soc_intel_is_byt() || soc_intel_is_cht()) {
		/* backward-compatibility for BYT/CHT boards */
		cpus->dai_name = devm_kasprintf(dev, GFP_KERNEL, "ssp%d-port",
						ssp_codec);
	} else {
		cpus->dai_name = devm_kasprintf(dev, GFP_KERNEL, "SSP%d Pin",
						ssp_codec);
	}
	if (!cpus->dai_name)
		return -ENOMEM;

	link->cpus = cpus;
	link->num_cpus = 1;

	/* codecs - caller to handle */

	/* platforms */
	link->platforms = platform_component;
	link->num_platforms = ARRAY_SIZE(platform_component);

	link->id = be_id;
	link->no_pcm = 1;
	link->dpcm_capture = 1;
	link->dpcm_playback = 1;

	return 0;
}
EXPORT_SYMBOL_NS(sof_intel_board_set_codec_link, SND_SOC_INTEL_SOF_BOARD_HELPERS);

int sof_intel_board_set_dmic_link(struct device *dev,
				  struct snd_soc_dai_link *link, int be_id,
				  enum sof_dmic_be_type be_type)
{
	struct snd_soc_dai_link_component *cpus;

	/* cpus */
	cpus = devm_kzalloc(dev, sizeof(struct snd_soc_dai_link_component),
			    GFP_KERNEL);
	if (!cpus)
		return -ENOMEM;

	switch (be_type) {
	case SOF_DMIC_01:
		dev_dbg(dev, "link %d: dmic01\n", be_id);

		link->name = "dmic01";
		cpus->dai_name = "DMIC01 Pin";
		break;
	case SOF_DMIC_16K:
		dev_dbg(dev, "link %d: dmic16k\n", be_id);

		link->name = "dmic16k";
		cpus->dai_name = "DMIC16k Pin";
		break;
	default:
		dev_err(dev, "invalid be type %d\n", be_type);
		return -EINVAL;
	}

	link->cpus = cpus;
	link->num_cpus = 1;

	/* codecs */
	link->codecs = dmic_component;
	link->num_codecs = ARRAY_SIZE(dmic_component);

	/* platforms */
	link->platforms = platform_component;
	link->num_platforms = ARRAY_SIZE(platform_component);

	link->id = be_id;
	if (be_type == SOF_DMIC_01)
		link->init = dmic_init;
	link->ignore_suspend = 1;
	link->no_pcm = 1;
	link->dpcm_capture = 1;

	return 0;
}
EXPORT_SYMBOL_NS(sof_intel_board_set_dmic_link, SND_SOC_INTEL_SOF_BOARD_HELPERS);

int sof_intel_board_set_intel_hdmi_link(struct device *dev,
					struct snd_soc_dai_link *link, int be_id,
					int hdmi_id, bool idisp_codec)
{
	struct snd_soc_dai_link_component *cpus, *codecs;

	dev_dbg(dev, "link %d: intel hdmi, hdmi id %d, idisp codec %d\n",
		be_id, hdmi_id, idisp_codec);

	/* link name */
	link->name = devm_kasprintf(dev, GFP_KERNEL, "iDisp%d", hdmi_id);
	if (!link->name)
		return -ENOMEM;

	/* cpus */
	cpus = devm_kzalloc(dev, sizeof(struct snd_soc_dai_link_component),
			    GFP_KERNEL);
	if (!cpus)
		return -ENOMEM;

	cpus->dai_name = devm_kasprintf(dev, GFP_KERNEL, "iDisp%d Pin", hdmi_id);
	if (!cpus->dai_name)
		return -ENOMEM;

	link->cpus = cpus;
	link->num_cpus = 1;

	/* codecs */
	if (idisp_codec) {
		codecs = devm_kzalloc(dev,
				      sizeof(struct snd_soc_dai_link_component),
				      GFP_KERNEL);
		if (!codecs)
			return -ENOMEM;

		codecs->name = "ehdaudio0D2";
		codecs->dai_name = devm_kasprintf(dev, GFP_KERNEL,
						  "intel-hdmi-hifi%d", hdmi_id);
		if (!codecs->dai_name)
			return -ENOMEM;

		link->codecs = codecs;
	} else {
		link->codecs = &snd_soc_dummy_dlc;
	}
	link->num_codecs = 1;

	/* platforms */
	link->platforms = platform_component;
	link->num_platforms = ARRAY_SIZE(platform_component);

	link->id = be_id;
	link->init = (hdmi_id == 1) ? hdmi_init : NULL;
	link->no_pcm = 1;
	link->dpcm_playback = 1;

	return 0;
}
EXPORT_SYMBOL_NS(sof_intel_board_set_intel_hdmi_link, SND_SOC_INTEL_SOF_BOARD_HELPERS);

int sof_intel_board_set_ssp_amp_link(struct device *dev,
				     struct snd_soc_dai_link *link, int be_id,
				     enum sof_ssp_codec amp_type, int ssp_amp)
{
	struct snd_soc_dai_link_component *cpus;

	dev_dbg(dev, "link %d: ssp amp %s, ssp %d\n", be_id,
		sof_ssp_get_codec_name(amp_type), ssp_amp);

	/* link name */
	link->name = devm_kasprintf(dev, GFP_KERNEL, "SSP%d-Codec", ssp_amp);
	if (!link->name)
		return -ENOMEM;

	/* cpus */
	cpus = devm_kzalloc(dev, sizeof(struct snd_soc_dai_link_component),
			    GFP_KERNEL);
	if (!cpus)
		return -ENOMEM;

	cpus->dai_name = devm_kasprintf(dev, GFP_KERNEL, "SSP%d Pin", ssp_amp);
	if (!cpus->dai_name)
		return -ENOMEM;

	link->cpus = cpus;
	link->num_cpus = 1;

	/* codecs - caller to handle */

	/* platforms */
	link->platforms = platform_component;
	link->num_platforms = ARRAY_SIZE(platform_component);

	link->id = be_id;
	link->no_pcm = 1;
	link->dpcm_capture = 1; /* feedback stream or firmware-generated echo reference */
	link->dpcm_playback = 1;

	return 0;
}
EXPORT_SYMBOL_NS(sof_intel_board_set_ssp_amp_link, SND_SOC_INTEL_SOF_BOARD_HELPERS);

int sof_intel_board_set_bt_link(struct device *dev,
				struct snd_soc_dai_link *link, int be_id,
				int ssp_bt)
{
	struct snd_soc_dai_link_component *cpus;

	dev_dbg(dev, "link %d: bt offload, ssp %d\n", be_id, ssp_bt);

	/* link name */
	link->name = devm_kasprintf(dev, GFP_KERNEL, "SSP%d-BT", ssp_bt);
	if (!link->name)
		return -ENOMEM;

	/* cpus */
	cpus = devm_kzalloc(dev, sizeof(struct snd_soc_dai_link_component),
			    GFP_KERNEL);
	if (!cpus)
		return -ENOMEM;

	cpus->dai_name = devm_kasprintf(dev, GFP_KERNEL, "SSP%d Pin", ssp_bt);
	if (!cpus->dai_name)
		return -ENOMEM;

	link->cpus = cpus;
	link->num_cpus = 1;

	/* codecs */
	link->codecs = &snd_soc_dummy_dlc;
	link->num_codecs = 1;

	/* platforms */
	link->platforms = platform_component;
	link->num_platforms = ARRAY_SIZE(platform_component);

	link->id = be_id;
	link->no_pcm = 1;
	link->dpcm_capture = 1;
	link->dpcm_playback = 1;

	return 0;
}
EXPORT_SYMBOL_NS(sof_intel_board_set_bt_link, SND_SOC_INTEL_SOF_BOARD_HELPERS);

int sof_intel_board_set_hdmi_in_link(struct device *dev,
				     struct snd_soc_dai_link *link, int be_id,
				     int ssp_hdmi)
{
	struct snd_soc_dai_link_component *cpus;

	dev_dbg(dev, "link %d: hdmi-in, ssp %d\n", be_id, ssp_hdmi);

	/* link name */
	link->name = devm_kasprintf(dev, GFP_KERNEL, "SSP%d-HDMI", ssp_hdmi);
	if (!link->name)
		return -ENOMEM;

	/* cpus */
	cpus = devm_kzalloc(dev, sizeof(struct snd_soc_dai_link_component),
			    GFP_KERNEL);
	if (!cpus)
		return -ENOMEM;

	cpus->dai_name = devm_kasprintf(dev, GFP_KERNEL, "SSP%d Pin", ssp_hdmi);
	if (!cpus->dai_name)
		return -ENOMEM;

	link->cpus = cpus;
	link->num_cpus = 1;

	/* codecs */
	link->codecs = &snd_soc_dummy_dlc;
	link->num_codecs = 1;

	/* platforms */
	link->platforms = platform_component;
	link->num_platforms = ARRAY_SIZE(platform_component);

	link->id = be_id;
	link->no_pcm = 1;
	link->dpcm_capture = 1;

	return 0;
}
EXPORT_SYMBOL_NS(sof_intel_board_set_hdmi_in_link, SND_SOC_INTEL_SOF_BOARD_HELPERS);

static int calculate_num_links(struct sof_card_private *ctx)
{
	int num_links = 0;

	/* headphone codec */
	if (ctx->codec_type != CODEC_NONE)
		num_links++;

	/* dmic01 and dmic16k */
	if (ctx->dmic_be_num > 0)
		num_links++;

	if (ctx->dmic_be_num > 1)
		num_links++;

	/* idisp HDMI */
	num_links += ctx->hdmi_num;

	/* speaker amp */
	if (ctx->amp_type != CODEC_NONE)
		num_links++;

	/* BT audio offload */
	if (ctx->bt_offload_present)
		num_links++;

	/* HDMI-In */
	num_links += hweight32(ctx->ssp_mask_hdmi_in);

	return num_links;
}

int sof_intel_board_set_dai_link(struct device *dev, struct snd_soc_card *card,
				 struct sof_card_private *ctx)
{
	struct snd_soc_dai_link *links;
	int num_links;
	int i;
	int idx = 0;
	int ret;
	int ssp_hdmi_in = 0;

	num_links = calculate_num_links(ctx);

	links = devm_kcalloc(dev, num_links, sizeof(struct snd_soc_dai_link),
			     GFP_KERNEL);
	if (!links)
		return -ENOMEM;

	/* headphone codec */
	if (ctx->codec_type != CODEC_NONE) {
		ret = sof_intel_board_set_codec_link(dev, &links[idx], idx,
						     ctx->codec_type,
						     ctx->ssp_codec);
		if (ret) {
			dev_err(dev, "fail to set codec link, ret %d\n", ret);
			return ret;
		}

		ctx->codec_link = &links[idx];
		idx++;
	}

	/* dmic01 and dmic16k */
	if (ctx->dmic_be_num > 0) {
		/* at least we have dmic01 */
		ret = sof_intel_board_set_dmic_link(dev, &links[idx], idx,
						    SOF_DMIC_01);
		if (ret) {
			dev_err(dev, "fail to set dmic01 link, ret %d\n", ret);
			return ret;
		}

		idx++;
	}

	if (ctx->dmic_be_num > 1) {
		/* set up 2 BE links at most */
		ret = sof_intel_board_set_dmic_link(dev, &links[idx], idx,
						    SOF_DMIC_16K);
		if (ret) {
			dev_err(dev, "fail to set dmic16k link, ret %d\n", ret);
			return ret;
		}

		idx++;
	}

	/* idisp HDMI */
	for (i = 1; i <= ctx->hdmi_num; i++) {
		ret = sof_intel_board_set_intel_hdmi_link(dev, &links[idx], idx,
							  i,
							  ctx->hdmi.idisp_codec);
		if (ret) {
			dev_err(dev, "fail to set hdmi link, ret %d\n", ret);
			return ret;
		}

		idx++;
	}

	/* speaker amp */
	if (ctx->amp_type != CODEC_NONE) {
		ret = sof_intel_board_set_ssp_amp_link(dev, &links[idx], idx,
						       ctx->amp_type,
						       ctx->ssp_amp);
		if (ret) {
			dev_err(dev, "fail to set amp link, ret %d\n", ret);
			return ret;
		}

		ctx->amp_link = &links[idx];
		idx++;
	}

	/* BT audio offload */
	if (ctx->bt_offload_present) {
		ret = sof_intel_board_set_bt_link(dev, &links[idx], idx,
						  ctx->ssp_bt);
		if (ret) {
			dev_err(dev, "fail to set bt link, ret %d\n", ret);
			return ret;
		}

		idx++;
	}

	/* HDMI-In */
	for_each_set_bit(ssp_hdmi_in, &ctx->ssp_mask_hdmi_in, 32) {
		ret = sof_intel_board_set_hdmi_in_link(dev, &links[idx], idx,
						       ssp_hdmi_in);
		if (ret) {
			dev_err(dev, "fail to set hdmi-in link, ret %d\n", ret);
			return ret;
		}

		idx++;
	}

	if (idx != num_links) {
		dev_err(dev, "link number mismatch, idx %d, num_links %d\n", idx,
			num_links);
		return -EINVAL;
	}

	card->dai_link = links;
	card->num_links = num_links;

	return 0;
}
EXPORT_SYMBOL_NS(sof_intel_board_set_dai_link, SND_SOC_INTEL_SOF_BOARD_HELPERS);

MODULE_DESCRIPTION("ASoC Intel SOF Machine Driver Board Helpers");
MODULE_AUTHOR("Brent Lu <brent.lu@intel.com>");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(SND_SOC_INTEL_HDA_DSP_COMMON);
MODULE_IMPORT_NS(SND_SOC_INTEL_SOF_SSP_COMMON);
