/* SPDX-License-Identifier: (GPL-2.0+ OR BSD-3-Clause) */
/*
 * This header file defines the register offsets and bit fields
 * of ENETC4 PF and VFs. Note that the same registers as ENETC
 * version 1.0 are defined in the enetc_hw.h file.
 *
 * Copyright 2024 NXP
 */
#ifndef __ENETC4_HW_H_
#define __ENETC4_HW_H_

#define NXP_ENETC_VENDOR_ID		0x1131
#define NXP_ENETC_PF_DEV_ID		0xe101

/***************************ENETC port registers**************************/
#define ENETC4_ECAPR0			0x0
#define  ECAPR0_RFS			BIT(2)
#define  ECAPR0_TSD			BIT(5)
#define  ECAPR0_RSS			BIT(8)
#define  ECAPR0_RSC			BIT(9)
#define  ECAPR0_LSO			BIT(10)
#define  ECAPR0_WO			BIT(13)

#define ENETC4_ECAPR1			0x4
#define  ECAPR1_NUM_TCS			GENMASK(6, 4)
#define  ECAPR1_NUM_MCH			GENMASK(9, 8)
#define  ECAPR1_NUM_UCH			GENMASK(11, 10)
#define  ECAPR1_NUM_MSIX		GENMASK(22, 12)
#define  ECAPR1_NUM_VSI			GENMASK(27, 24)
#define  ECAPR1_NUM_IPV			BIT(31)

#define ENETC4_ECAPR2			0x8
#define  ECAPR2_NUM_TX_BDR		GENMASK(9, 0)
#define  ECAPR2_NUM_RX_BDR		GENMASK(25, 16)

#define ENETC4_PMR			0x10
#define  PMR_SI_EN(a)			BIT((16 + (a)))

/* Port Pause ON/OFF threshold register */
#define ENETC4_PPAUONTR			0x108
#define ENETC4_PPAUOFFTR		0x10c

/* Port Station interface promiscuous MAC mode register */
#define ENETC4_PSIPMMR			0x200
#define  PSIPMMR_SI_MAC_UP(a)		BIT(a) /* a = SI index */
#define  PSIPMMR_SI_MAC_MP(a)		BIT((a) + 16)

/* Port Station interface promiscuous VLAN mode register */
#define ENETC4_PSIPVMR			0x204

/* Port RSS key register n. n = 0,1,2,...,9 */
#define ENETC4_PRSSKR(n)		((n) * 0x4 + 0x250)

/* Port station interface MAC address filtering capability register */
#define ENETC4_PSIMAFCAPR		0x280
#define  PSIMAFCAPR_NUM_MAC_AFTE	GENMASK(11, 0)

/* Port station interface VLAN filtering capability register */
#define ENETC4_PSIVLANFCAPR		0x2c0
#define  PSIVLANFCAPR_NUM_VLAN_FTE	GENMASK(11, 0)

/* Port station interface VLAN filtering mode register */
#define ENETC4_PSIVLANFMR		0x2c4
#define  PSIVLANFMR_VS			BIT(0)

/* Port Station interface a primary MAC address registers */
#define ENETC4_PSIPMAR0(a)		((a) * 0x80 + 0x2000)
#define ENETC4_PSIPMAR1(a)		((a) * 0x80 + 0x2004)

/* Port station interface a configuration register 0/2 */
#define ENETC4_PSICFGR0(a)		((a) * 0x80 + 0x2010)
#define  PSICFGR0_VASE			BIT(13)
#define  PSICFGR0_ASE			BIT(15)
#define  PSICFGR0_ANTI_SPOOFING		(PSICFGR0_VASE | PSICFGR0_ASE)

#define ENETC4_PSICFGR2(a)		((a) * 0x80 + 0x2018)
#define  PSICFGR2_NUM_MSIX		GENMASK(5, 0)

#define ENETC4_PMCAPR			0x4004
#define  PMCAPR_HD			BIT(8)
#define  PMCAPR_FP			GENMASK(10, 9)

/* Port configuration register */
#define ENETC4_PCR			0x4010
#define  PCR_HDR_FMT			BIT(0)
#define  PCR_L2DOSE			BIT(4)
#define  PCR_TIMER_CS			BIT(8)
#define  PCR_PSPEED			GENMASK(29, 16)
#define  PCR_PSPEED_VAL(speed)		(((speed) / 10 - 1) << 16)

/* Port MAC address register 0/1 */
#define ENETC4_PMAR0			0x4020
#define ENETC4_PMAR1			0x4024

/* Port operational register */
#define ENETC4_POR			0x4100

/* Port traffic class a transmit maximum SDU register */
#define ENETC4_PTCTMSDUR(a)		((a) * 0x20 + 0x4208)
#define  PTCTMSDUR_MAXSDU		GENMASK(15, 0)
#define  PTCTMSDUR_SDU_TYPE		GENMASK(17, 16)
#define   SDU_TYPE_PPDU			0
#define   SDU_TYPE_MPDU			1
#define   SDU_TYPE_MSDU			2

#define ENETC4_PMAC_OFFSET		0x400
#define ENETC4_PM_CMD_CFG(mac)		(0x5008 + (mac) * 0x400)
#define  PM_CMD_CFG_TX_EN		BIT(0)
#define  PM_CMD_CFG_RX_EN		BIT(1)
#define  PM_CMD_CFG_PAUSE_FWD		BIT(7)
#define  PM_CMD_CFG_PAUSE_IGN		BIT(8)
#define  PM_CMD_CFG_TX_ADDR_INS		BIT(9)
#define  PM_CMD_CFG_LOOP_EN		BIT(10)
#define  PM_CMD_CFG_LPBK_MODE		GENMASK(12, 11)
#define   LPBCK_MODE_EXT_TX_CLK		0
#define   LPBCK_MODE_MAC_LEVEL		1
#define   LPBCK_MODE_INT_TX_CLK		2
#define  PM_CMD_CFG_CNT_FRM_EN		BIT(13)
#define  PM_CMD_CFG_TXP			BIT(15)
#define  PM_CMD_CFG_SEND_IDLE		BIT(16)
#define  PM_CMD_CFG_HD_FCEN		BIT(18)
#define  PM_CMD_CFG_SFD			BIT(21)
#define  PM_CMD_CFG_TX_FLUSH		BIT(22)
#define  PM_CMD_CFG_TX_LOWP_EN		BIT(23)
#define  PM_CMD_CFG_RX_LOWP_EMPTY	BIT(24)
#define  PM_CMD_CFG_SWR			BIT(26)
#define  PM_CMD_CFG_TS_MODE		BIT(30)
#define  PM_CMD_CFG_MG			BIT(31)

/* Port MAC 0/1 Maximum Frame Length Register */
#define ENETC4_PM_MAXFRM(mac)		(0x5014 + (mac) * 0x400)

/* Port MAC 0/1 Pause Quanta Register */
#define ENETC4_PM_PAUSE_QUANTA(mac)	(0x5054 + (mac) * 0x400)

/* Port MAC 0/1 Pause Quanta Threshold Register */
#define ENETC4_PM_PAUSE_THRESH(mac)	(0x5064 + (mac) * 0x400)

/* Port MAC 0 Interface Mode Control Register */
#define ENETC4_PM_IF_MODE(mac)		(0x5300 + (mac) * 0x400)
#define  PM_IF_MODE_IFMODE		GENMASK(2, 0)
#define   IFMODE_XGMII			0
#define   IFMODE_RMII			3
#define   IFMODE_RGMII			4
#define   IFMODE_SGMII			5
#define  PM_IF_MODE_REVMII		BIT(3)
#define  PM_IF_MODE_M10			BIT(4)
#define  PM_IF_MODE_HD			BIT(6)
#define  PM_IF_MODE_SSP			GENMASK(14, 13)
#define   SSP_100M			0
#define   SSP_10M			1
#define   SSP_1G			2
#define  PM_IF_MODE_ENA			BIT(15)

#endif
