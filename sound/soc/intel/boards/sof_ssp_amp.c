// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright(c) 2022 Intel Corporation. All rights reserved.

/*
 * sof_ssp_amp.c - ASoc Machine driver for Intel platforms
 * with RT1308/CS35L41 codec.
 */

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <sound/core.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/sof.h>
#include "sof_board_helpers.h"
#include "sof_realtek_common.h"
#include "sof_cirrus_common.h"
#include "sof_ssp_common.h"

/* SSP port ID for speaker amplifier */
#define SOF_AMPLIFIER_SSP(quirk)		((quirk) & GENMASK(3, 0))
#define SOF_AMPLIFIER_SSP_MASK			(GENMASK(3, 0))

/* HDMI capture*/
#define SOF_SSP_HDMI_CAPTURE_PRESENT		BIT(4)
#define SOF_NO_OF_HDMI_CAPTURE_SSP_SHIFT		5
#define SOF_NO_OF_HDMI_CAPTURE_SSP_MASK		(GENMASK(6, 5))
#define SOF_NO_OF_HDMI_CAPTURE_SSP(quirk)	\
	(((quirk) << SOF_NO_OF_HDMI_CAPTURE_SSP_SHIFT) & SOF_NO_OF_HDMI_CAPTURE_SSP_MASK)

#define SOF_HDMI_CAPTURE_1_SSP_SHIFT		7
#define SOF_HDMI_CAPTURE_1_SSP_MASK		(GENMASK(9, 7))
#define SOF_HDMI_CAPTURE_1_SSP(quirk)	\
	(((quirk) << SOF_HDMI_CAPTURE_1_SSP_SHIFT) & SOF_HDMI_CAPTURE_1_SSP_MASK)

#define SOF_HDMI_CAPTURE_2_SSP_SHIFT		10
#define SOF_HDMI_CAPTURE_2_SSP_MASK		(GENMASK(12, 10))
#define SOF_HDMI_CAPTURE_2_SSP(quirk)	\
	(((quirk) << SOF_HDMI_CAPTURE_2_SSP_SHIFT) & SOF_HDMI_CAPTURE_2_SSP_MASK)

/* HDMI playback */
#define SOF_HDMI_PLAYBACK_PRESENT		BIT(13)
#define SOF_NO_OF_HDMI_PLAYBACK_SHIFT		14
#define SOF_NO_OF_HDMI_PLAYBACK_MASK		(GENMASK(16, 14))
#define SOF_NO_OF_HDMI_PLAYBACK(quirk)	\
	(((quirk) << SOF_NO_OF_HDMI_PLAYBACK_SHIFT) & SOF_NO_OF_HDMI_PLAYBACK_MASK)

/* BT audio offload */
#define SOF_SSP_BT_OFFLOAD_PRESENT		BIT(17)
#define SOF_BT_OFFLOAD_SSP_SHIFT		18
#define SOF_BT_OFFLOAD_SSP_MASK			(GENMASK(20, 18))
#define SOF_BT_OFFLOAD_SSP(quirk)	\
	(((quirk) << SOF_BT_OFFLOAD_SSP_SHIFT) & SOF_BT_OFFLOAD_SSP_MASK)

/* Default: SSP2  */
static unsigned long sof_ssp_amp_quirk = SOF_AMPLIFIER_SSP(2);

static const struct dmi_system_id chromebook_platforms[] = {
	{
		.ident = "Google Chromebooks",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Google"),
		}
	},
	{},
};

static int sof_card_late_probe(struct snd_soc_card *card)
{
	return sof_intel_board_card_late_probe(card);
}

static struct snd_soc_card sof_ssp_amp_card = {
	.name         = "ssp_amp",
	.owner        = THIS_MODULE,
	.fully_routed = true,
	.late_probe = sof_card_late_probe,
};

static struct snd_soc_dai_link_component platform_component[] = {
	{
		/* name might be overridden during probe */
		.name = "0000:00:1f.3"
	}
};

/* BE ID defined in sof-tgl-rt1308-hdmi-ssp.m4 */
#define HDMI_IN_BE_ID		0
#define SPK_BE_ID		2
#define DMIC01_BE_ID		3
#define DMIC16K_BE_ID		4
#define INTEL_HDMI_BE_ID	5

static struct snd_soc_dai_link *
sof_card_dai_links_create(struct device *dev, enum sof_ssp_codec amp_type,
			  int ssp_codec, int dmic_be_num, int hdmi_num,
			  bool idisp_codec)
{
	struct snd_soc_dai_link_component *cpus;
	struct snd_soc_dai_link *links;
	int i;
	int id = 0;
	int ret;
	bool fixed_be = false;
	int be_id;

	links = devm_kcalloc(dev, sof_ssp_amp_card.num_links,
					sizeof(struct snd_soc_dai_link), GFP_KERNEL);
	cpus = devm_kcalloc(dev, sof_ssp_amp_card.num_links,
					sizeof(struct snd_soc_dai_link_component), GFP_KERNEL);
	if (!links || !cpus)
		return NULL;

	/* HDMI-In SSP */
	if (sof_ssp_amp_quirk & SOF_SSP_HDMI_CAPTURE_PRESENT) {
		int num_of_hdmi_ssp = (sof_ssp_amp_quirk & SOF_NO_OF_HDMI_CAPTURE_SSP_MASK) >>
				SOF_NO_OF_HDMI_CAPTURE_SSP_SHIFT;

		/* the topology supports HDMI-IN uses fixed BE ID for DAI links */
		fixed_be = true;

		for (i = 1; i <= num_of_hdmi_ssp; i++) {
			int port = (i == 1 ? (sof_ssp_amp_quirk & SOF_HDMI_CAPTURE_1_SSP_MASK) >>
						SOF_HDMI_CAPTURE_1_SSP_SHIFT :
						(sof_ssp_amp_quirk & SOF_HDMI_CAPTURE_2_SSP_MASK) >>
						SOF_HDMI_CAPTURE_2_SSP_SHIFT);

			links[id].cpus = &cpus[id];
			links[id].cpus->dai_name = devm_kasprintf(dev, GFP_KERNEL,
								  "SSP%d Pin", port);
			if (!links[id].cpus->dai_name)
				return NULL;
			links[id].name = devm_kasprintf(dev, GFP_KERNEL, "SSP%d-HDMI", port);
			if (!links[id].name)
				return NULL;
			links[id].id = fixed_be ? (HDMI_IN_BE_ID + i - 1) : id;
			links[id].codecs = &snd_soc_dummy_dlc;
			links[id].num_codecs = 1;
			links[id].platforms = platform_component;
			links[id].num_platforms = ARRAY_SIZE(platform_component);
			links[id].dpcm_capture = 1;
			links[id].no_pcm = 1;
			links[id].num_cpus = 1;
			id++;
		}
	}

	/* codec SSP */
	if (amp_type != CODEC_NONE) {
		links[id].name = devm_kasprintf(dev, GFP_KERNEL, "SSP%d-Codec", ssp_codec);
		if (!links[id].name)
			return NULL;

		links[id].id = fixed_be ? SPK_BE_ID : id;

		switch (amp_type) {
		case CODEC_CS35L41:
			cs35l41_set_dai_link(&links[id]);
			break;
		case CODEC_RT1308:
			sof_rt1308_dai_link(&links[id]);
			break;
		default:
			dev_err(dev, "invalid amp type %d\n", amp_type);
			return NULL;
		}

		links[id].platforms = platform_component;
		links[id].num_platforms = ARRAY_SIZE(platform_component);
		links[id].dpcm_playback = 1;
		/* feedback from amplifier or firmware-generated echo reference */
		links[id].dpcm_capture = 1;
		links[id].no_pcm = 1;
		links[id].cpus = &cpus[id];
		links[id].num_cpus = 1;
		links[id].cpus->dai_name = devm_kasprintf(dev, GFP_KERNEL, "SSP%d Pin", ssp_codec);
		if (!links[id].cpus->dai_name)
			return NULL;

		id++;
	}

	/* dmic */
	if (dmic_be_num > 0) {
		/* at least we have dmic01 */
		be_id = fixed_be ? DMIC01_BE_ID : id;
		ret = sof_intel_board_set_dmic_link(dev, &links[id], be_id,
						    SOF_DMIC_01);
		if (ret)
			return NULL;

		id++;
	}

	if (dmic_be_num > 1) {
		/* set up 2 BE links at most */
		be_id = fixed_be ? DMIC16K_BE_ID : id;
		ret = sof_intel_board_set_dmic_link(dev, &links[id], be_id,
						    SOF_DMIC_16K);
		if (ret)
			return NULL;

		id++;
	}

	/* HDMI playback */
	for (i = 1; i <= hdmi_num; i++) {
		be_id = fixed_be ? (INTEL_HDMI_BE_ID + i - 1) : id;
		ret = sof_intel_board_set_intel_hdmi_link(dev, &links[id], be_id,
							  i, idisp_codec);
		if (ret)
			return NULL;

		id++;
	}

	/* BT audio offload */
	if (sof_ssp_amp_quirk & SOF_SSP_BT_OFFLOAD_PRESENT) {
		int port = (sof_ssp_amp_quirk & SOF_BT_OFFLOAD_SSP_MASK) >>
				SOF_BT_OFFLOAD_SSP_SHIFT;

		links[id].id = id;
		links[id].cpus = &cpus[id];
		links[id].cpus->dai_name = devm_kasprintf(dev, GFP_KERNEL,
							  "SSP%d Pin", port);
		if (!links[id].cpus->dai_name)
			goto devm_err;
		links[id].name = devm_kasprintf(dev, GFP_KERNEL, "SSP%d-BT", port);
		if (!links[id].name)
			goto devm_err;
		links[id].codecs = &snd_soc_dummy_dlc;
		links[id].num_codecs = 1;
		links[id].platforms = platform_component;
		links[id].num_platforms = ARRAY_SIZE(platform_component);
		links[id].dpcm_playback = 1;
		links[id].dpcm_capture = 1;
		links[id].no_pcm = 1;
		links[id].num_cpus = 1;
		id++;
	}

	return links;
devm_err:
	return NULL;
}

static int sof_ssp_amp_probe(struct platform_device *pdev)
{
	struct snd_soc_acpi_mach *mach = pdev->dev.platform_data;
	struct snd_soc_dai_link *dai_links;
	struct sof_card_private *ctx;
	int ret, ssp_codec;

	ctx = devm_kzalloc(&pdev->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	if (pdev->id_entry && pdev->id_entry->driver_data)
		sof_ssp_amp_quirk = (unsigned long)pdev->id_entry->driver_data;

	ctx->amp_type = sof_ssp_detect_amp_type(&pdev->dev);

	if (dmi_check_system(chromebook_platforms) || mach->mach_params.dmic_num > 0)
		ctx->dmic_be_num = 2;
	else
		ctx->dmic_be_num = 0;

	ssp_codec = sof_ssp_amp_quirk & SOF_AMPLIFIER_SSP_MASK;

	/* set number of dai links */
	sof_ssp_amp_card.num_links = ctx->dmic_be_num;

	if (ctx->amp_type != CODEC_NONE)
		sof_ssp_amp_card.num_links++;

	if (sof_ssp_amp_quirk & SOF_SSP_HDMI_CAPTURE_PRESENT)
		sof_ssp_amp_card.num_links += (sof_ssp_amp_quirk & SOF_NO_OF_HDMI_CAPTURE_SSP_MASK) >>
				SOF_NO_OF_HDMI_CAPTURE_SSP_SHIFT;

	if (sof_ssp_amp_quirk & SOF_HDMI_PLAYBACK_PRESENT) {
		ctx->hdmi_num = (sof_ssp_amp_quirk & SOF_NO_OF_HDMI_PLAYBACK_MASK) >>
				SOF_NO_OF_HDMI_PLAYBACK_SHIFT;
		/* default number of HDMI DAI's */
		if (!ctx->hdmi_num)
			ctx->hdmi_num = 3;

		if (mach->mach_params.codec_mask & IDISP_CODEC_MASK)
			ctx->hdmi.idisp_codec = true;

		sof_ssp_amp_card.num_links += ctx->hdmi_num;
	} else {
		ctx->hdmi_num = 0;
	}

	if (sof_ssp_amp_quirk & SOF_SSP_BT_OFFLOAD_PRESENT)
		sof_ssp_amp_card.num_links++;

	dai_links = sof_card_dai_links_create(&pdev->dev, ctx->amp_type,
					      ssp_codec, ctx->dmic_be_num,
					      ctx->hdmi_num,
					      ctx->hdmi.idisp_codec);
	if (!dai_links)
		return -ENOMEM;

	sof_ssp_amp_card.dai_link = dai_links;

	/* update codec_conf */
	switch (ctx->amp_type) {
	case CODEC_CS35L41:
		cs35l41_set_codec_conf(&sof_ssp_amp_card);
		break;
	case CODEC_NONE:
	case CODEC_RT1308:
		/* no codec conf required */
		break;
	default:
		dev_err(&pdev->dev, "invalid amp type %d\n", ctx->amp_type);
		return -EINVAL;
	}

	sof_ssp_amp_card.dev = &pdev->dev;

	/* set platform name for each dailink */
	ret = snd_soc_fixup_dai_links_platform_name(&sof_ssp_amp_card,
						    mach->mach_params.platform);
	if (ret)
		return ret;

	snd_soc_card_set_drvdata(&sof_ssp_amp_card, ctx);

	return devm_snd_soc_register_card(&pdev->dev, &sof_ssp_amp_card);
}

static const struct platform_device_id board_ids[] = {
	{
		.name = "sof_ssp_amp",
	},
	{
		.name = "tgl_rt1308_hdmi_ssp",
		.driver_data = (kernel_ulong_t)(SOF_AMPLIFIER_SSP(2) |
					SOF_NO_OF_HDMI_CAPTURE_SSP(2) |
					SOF_HDMI_CAPTURE_1_SSP(1) |
					SOF_HDMI_CAPTURE_2_SSP(5) |
					SOF_SSP_HDMI_CAPTURE_PRESENT),
	},
	{
		.name = "adl_cs35l41",
		.driver_data = (kernel_ulong_t)(SOF_AMPLIFIER_SSP(1) |
					SOF_NO_OF_HDMI_PLAYBACK(4) |
					SOF_HDMI_PLAYBACK_PRESENT |
					SOF_BT_OFFLOAD_SSP(2) |
					SOF_SSP_BT_OFFLOAD_PRESENT),
	},
	{
		.name = "adl_lt6911_hdmi_ssp",
		.driver_data = (kernel_ulong_t)(SOF_NO_OF_HDMI_CAPTURE_SSP(2) |
					SOF_HDMI_CAPTURE_1_SSP(0) |
					SOF_HDMI_CAPTURE_2_SSP(2) |
					SOF_SSP_HDMI_CAPTURE_PRESENT |
					SOF_NO_OF_HDMI_PLAYBACK(3) |
					SOF_HDMI_PLAYBACK_PRESENT),
	},
	{
		.name = "rpl_lt6911_hdmi_ssp",
		.driver_data = (kernel_ulong_t)(SOF_NO_OF_HDMI_CAPTURE_SSP(2) |
					SOF_HDMI_CAPTURE_1_SSP(0) |
					SOF_HDMI_CAPTURE_2_SSP(2) |
					SOF_SSP_HDMI_CAPTURE_PRESENT |
					SOF_NO_OF_HDMI_PLAYBACK(3) |
					SOF_HDMI_PLAYBACK_PRESENT),
	},
	{
		.name = "mtl_lt6911_hdmi_ssp",
		.driver_data = (kernel_ulong_t)(SOF_NO_OF_HDMI_CAPTURE_SSP(2) |
				SOF_HDMI_CAPTURE_1_SSP(0) |
				SOF_HDMI_CAPTURE_2_SSP(2) |
				SOF_SSP_HDMI_CAPTURE_PRESENT |
				SOF_NO_OF_HDMI_PLAYBACK(3) |
				SOF_HDMI_PLAYBACK_PRESENT),
	},
	{ }
};
MODULE_DEVICE_TABLE(platform, board_ids);

static struct platform_driver sof_ssp_amp_driver = {
	.probe          = sof_ssp_amp_probe,
	.driver = {
		.name   = "sof_ssp_amp",
		.pm = &snd_soc_pm_ops,
	},
	.id_table = board_ids,
};
module_platform_driver(sof_ssp_amp_driver);

MODULE_DESCRIPTION("ASoC Intel(R) SOF Amplifier Machine driver");
MODULE_AUTHOR("Balamurugan C <balamurugan.c@intel.com>");
MODULE_AUTHOR("Brent Lu <brent.lu@intel.com>");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(SND_SOC_INTEL_SOF_BOARD_HELPERS);
MODULE_IMPORT_NS(SND_SOC_INTEL_SOF_REALTEK_COMMON);
MODULE_IMPORT_NS(SND_SOC_INTEL_SOF_CIRRUS_COMMON);
MODULE_IMPORT_NS(SND_SOC_INTEL_SOF_SSP_COMMON);
