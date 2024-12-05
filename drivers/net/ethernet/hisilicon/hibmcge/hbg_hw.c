// SPDX-License-Identifier: GPL-2.0+
// Copyright (c) 2024 Hisilicon Limited.

#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/iopoll.h>
#include <linux/minmax.h>
#include "hbg_common.h"
#include "hbg_hw.h"
#include "hbg_reg.h"

#define HBG_HW_EVENT_WAIT_TIMEOUT_US	(2 * 1000 * 1000)
#define HBG_HW_EVENT_WAIT_INTERVAL_US	(10 * 1000)
/* little endian or big endian.
 * ctrl means packet description, data means skb packet data
 */
#define HBG_ENDIAN_CTRL_LE_DATA_BE	0x0
#define HBG_PCU_FRAME_LEN_PLUS 4

static bool hbg_hw_spec_is_valid(struct hbg_priv *priv)
{
	return hbg_reg_read(priv, HBG_REG_SPEC_VALID_ADDR) &&
	       !hbg_reg_read(priv, HBG_REG_EVENT_REQ_ADDR);
}

int hbg_hw_event_notify(struct hbg_priv *priv,
			enum hbg_hw_event_type event_type)
{
	bool is_valid;
	int ret;

	if (test_and_set_bit(HBG_NIC_STATE_EVENT_HANDLING, &priv->state))
		return -EBUSY;

	/* notify */
	hbg_reg_write(priv, HBG_REG_EVENT_REQ_ADDR, event_type);

	ret = read_poll_timeout(hbg_hw_spec_is_valid, is_valid, is_valid,
				HBG_HW_EVENT_WAIT_INTERVAL_US,
				HBG_HW_EVENT_WAIT_TIMEOUT_US,
				HBG_HW_EVENT_WAIT_INTERVAL_US, priv);

	clear_bit(HBG_NIC_STATE_EVENT_HANDLING, &priv->state);

	if (ret)
		dev_err(&priv->pdev->dev,
			"event %d wait timeout\n", event_type);

	return ret;
}

static int hbg_hw_dev_specs_init(struct hbg_priv *priv)
{
	struct hbg_dev_specs *specs = &priv->dev_specs;
	u64 mac_addr;

	if (!hbg_hw_spec_is_valid(priv)) {
		dev_err(&priv->pdev->dev, "dev_specs not init\n");
		return -EINVAL;
	}

	specs->mac_id = hbg_reg_read(priv, HBG_REG_MAC_ID_ADDR);
	specs->phy_addr = hbg_reg_read(priv, HBG_REG_PHY_ID_ADDR);
	specs->mdio_frequency = hbg_reg_read(priv, HBG_REG_MDIO_FREQ_ADDR);
	specs->max_mtu = hbg_reg_read(priv, HBG_REG_MAX_MTU_ADDR);
	specs->min_mtu = hbg_reg_read(priv, HBG_REG_MIN_MTU_ADDR);
	specs->vlan_layers = hbg_reg_read(priv, HBG_REG_VLAN_LAYERS_ADDR);
	specs->rx_fifo_num = hbg_reg_read(priv, HBG_REG_RX_FIFO_NUM_ADDR);
	specs->tx_fifo_num = hbg_reg_read(priv, HBG_REG_TX_FIFO_NUM_ADDR);
	mac_addr = hbg_reg_read64(priv, HBG_REG_MAC_ADDR_ADDR);
	u64_to_ether_addr(mac_addr, (u8 *)specs->mac_addr.sa_data);

	if (!is_valid_ether_addr((u8 *)specs->mac_addr.sa_data))
		return -EADDRNOTAVAIL;

	specs->max_frame_len = HBG_PCU_CACHE_LINE_SIZE + specs->max_mtu;
	specs->rx_buf_size = HBG_PACKET_HEAD_SIZE + specs->max_frame_len;
	return 0;
}

u32 hbg_hw_get_irq_status(struct hbg_priv *priv)
{
	u32 status;

	status = hbg_reg_read(priv, HBG_REG_CF_INTRPT_STAT_ADDR);

	hbg_field_modify(status, HBG_INT_MSK_TX_B,
			 hbg_reg_read(priv, HBG_REG_CF_IND_TXINT_STAT_ADDR));
	hbg_field_modify(status, HBG_INT_MSK_RX_B,
			 hbg_reg_read(priv, HBG_REG_CF_IND_RXINT_STAT_ADDR));

	return status;
}

void hbg_hw_irq_clear(struct hbg_priv *priv, u32 mask)
{
	if (FIELD_GET(HBG_INT_MSK_TX_B, mask))
		return hbg_reg_write(priv, HBG_REG_CF_IND_TXINT_CLR_ADDR, 0x1);

	if (FIELD_GET(HBG_INT_MSK_RX_B, mask))
		return hbg_reg_write(priv, HBG_REG_CF_IND_RXINT_CLR_ADDR, 0x1);

	return hbg_reg_write(priv, HBG_REG_CF_INTRPT_CLR_ADDR, mask);
}

bool hbg_hw_irq_is_enabled(struct hbg_priv *priv, u32 mask)
{
	if (FIELD_GET(HBG_INT_MSK_TX_B, mask))
		return hbg_reg_read(priv, HBG_REG_CF_IND_TXINT_MSK_ADDR);

	if (FIELD_GET(HBG_INT_MSK_RX_B, mask))
		return hbg_reg_read(priv, HBG_REG_CF_IND_RXINT_MSK_ADDR);

	return hbg_reg_read(priv, HBG_REG_CF_INTRPT_MSK_ADDR) & mask;
}

void hbg_hw_irq_enable(struct hbg_priv *priv, u32 mask, bool enable)
{
	u32 value;

	if (FIELD_GET(HBG_INT_MSK_TX_B, mask))
		return hbg_reg_write(priv,
				     HBG_REG_CF_IND_TXINT_MSK_ADDR, enable);

	if (FIELD_GET(HBG_INT_MSK_RX_B, mask))
		return hbg_reg_write(priv,
				     HBG_REG_CF_IND_RXINT_MSK_ADDR, enable);

	value = hbg_reg_read(priv, HBG_REG_CF_INTRPT_MSK_ADDR);
	if (enable)
		value |= mask;
	else
		value &= ~mask;

	hbg_reg_write(priv, HBG_REG_CF_INTRPT_MSK_ADDR, value);
}

void hbg_hw_set_uc_addr(struct hbg_priv *priv, u64 mac_addr)
{
	hbg_reg_write64(priv, HBG_REG_STATION_ADDR_LOW_2_ADDR, mac_addr);
}

static void hbg_hw_set_pcu_max_frame_len(struct hbg_priv *priv,
					 u16 max_frame_len)
{
	max_frame_len = max_t(u32, max_frame_len, ETH_DATA_LEN);

	/* lower two bits of value must be set to 0 */
	max_frame_len = round_up(max_frame_len, HBG_PCU_FRAME_LEN_PLUS);

	hbg_reg_write_field(priv, HBG_REG_MAX_FRAME_LEN_ADDR,
			    HBG_REG_MAX_FRAME_LEN_M, max_frame_len);
}

static void hbg_hw_set_mac_max_frame_len(struct hbg_priv *priv,
					 u16 max_frame_size)
{
	hbg_reg_write_field(priv, HBG_REG_MAX_FRAME_SIZE_ADDR,
			    HBG_REG_MAX_FRAME_LEN_M, max_frame_size);
}

void hbg_hw_set_mtu(struct hbg_priv *priv, u16 mtu)
{
	hbg_hw_set_pcu_max_frame_len(priv, mtu);
	hbg_hw_set_mac_max_frame_len(priv, mtu);
}

void hbg_hw_mac_enable(struct hbg_priv *priv, u32 enable)
{
	hbg_reg_write_field(priv, HBG_REG_PORT_ENABLE_ADDR,
			    HBG_REG_PORT_ENABLE_TX_B, enable);
	hbg_reg_write_field(priv, HBG_REG_PORT_ENABLE_ADDR,
			    HBG_REG_PORT_ENABLE_RX_B, enable);
}

u32 hbg_hw_get_fifo_used_num(struct hbg_priv *priv, enum hbg_dir dir)
{
	if (dir & HBG_DIR_TX)
		return hbg_reg_read_field(priv, HBG_REG_CF_CFF_DATA_NUM_ADDR,
					  HBG_REG_CF_CFF_DATA_NUM_ADDR_TX_M);

	if (dir & HBG_DIR_RX)
		return hbg_reg_read_field(priv, HBG_REG_CF_CFF_DATA_NUM_ADDR,
					  HBG_REG_CF_CFF_DATA_NUM_ADDR_RX_M);

	return 0;
}

void hbg_hw_set_tx_desc(struct hbg_priv *priv, struct hbg_tx_desc *tx_desc)
{
	hbg_reg_write(priv, HBG_REG_TX_CFF_ADDR_0_ADDR, tx_desc->word0);
	hbg_reg_write(priv, HBG_REG_TX_CFF_ADDR_1_ADDR, tx_desc->word1);
	hbg_reg_write(priv, HBG_REG_TX_CFF_ADDR_2_ADDR, tx_desc->word2);
	hbg_reg_write(priv, HBG_REG_TX_CFF_ADDR_3_ADDR, tx_desc->word3);
}

void hbg_hw_fill_buffer(struct hbg_priv *priv, u32 buffer_dma_addr)
{
	hbg_reg_write(priv, HBG_REG_RX_CFF_ADDR_ADDR, buffer_dma_addr);
}

void hbg_hw_adjust_link(struct hbg_priv *priv, u32 speed, u32 duplex)
{
	hbg_reg_write_field(priv, HBG_REG_PORT_MODE_ADDR,
			    HBG_REG_PORT_MODE_M, speed);
	hbg_reg_write_field(priv, HBG_REG_DUPLEX_TYPE_ADDR,
			    HBG_REG_DUPLEX_B, duplex);
}

static void hbg_hw_init_transmit_ctrl(struct hbg_priv *priv)
{
	u32 ctrl = 0;

	ctrl |= FIELD_PREP(HBG_REG_TRANSMIT_CTRL_AN_EN_B, HBG_STATUS_ENABLE);
	ctrl |= FIELD_PREP(HBG_REG_TRANSMIT_CTRL_CRC_ADD_B, HBG_STATUS_ENABLE);
	ctrl |= FIELD_PREP(HBG_REG_TRANSMIT_CTRL_PAD_EN_B, HBG_STATUS_ENABLE);

	hbg_reg_write(priv, HBG_REG_TRANSMIT_CTRL_ADDR, ctrl);
}

static void hbg_hw_init_rx_ctrl(struct hbg_priv *priv)
{
	u32 ctrl = 0;

	ctrl |= FIELD_PREP(HBG_REG_RX_CTRL_RX_GET_ADDR_MODE_B,
			   HBG_STATUS_ENABLE);
	ctrl |= FIELD_PREP(HBG_REG_RX_CTRL_TIME_INF_EN_B, HBG_STATUS_DISABLE);
	ctrl |= FIELD_PREP(HBG_REG_RX_CTRL_RXBUF_1ST_SKIP_SIZE_M, HBG_RX_SKIP1);
	ctrl |= FIELD_PREP(HBG_REG_RX_CTRL_RXBUF_1ST_SKIP_SIZE2_M,
			   HBG_RX_SKIP2);
	ctrl |= FIELD_PREP(HBG_REG_RX_CTRL_RX_ALIGN_NUM_M, NET_IP_ALIGN);
	ctrl |= FIELD_PREP(HBG_REG_RX_CTRL_PORT_NUM, priv->dev_specs.mac_id);

	hbg_reg_write(priv, HBG_REG_RX_CTRL_ADDR, ctrl);
}

static void hbg_hw_init_rx_control(struct hbg_priv *priv)
{
	hbg_hw_init_rx_ctrl(priv);

	/* parse from L2 layer */
	hbg_reg_write_field(priv, HBG_REG_RX_PKT_MODE_ADDR,
			    HBG_REG_RX_PKT_MODE_PARSE_MODE_M, 0x1);

	hbg_reg_write_field(priv, HBG_REG_RECV_CTRL_ADDR,
			    HBG_REG_RECV_CTRL_STRIP_PAD_EN_B,
			    HBG_STATUS_ENABLE);
	hbg_reg_write_field(priv, HBG_REG_RX_BUF_SIZE_ADDR,
			    HBG_REG_RX_BUF_SIZE_M, priv->dev_specs.rx_buf_size);
	hbg_reg_write_field(priv, HBG_REG_CF_CRC_STRIP_ADDR,
			    HBG_REG_CF_CRC_STRIP_B, HBG_STATUS_DISABLE);
}

int hbg_hw_init(struct hbg_priv *priv)
{
	int ret;

	ret = hbg_hw_dev_specs_init(priv);
	if (ret)
		return ret;

	hbg_reg_write_field(priv, HBG_REG_BUS_CTRL_ADDR,
			    HBG_REG_BUS_CTRL_ENDIAN_M,
			    HBG_ENDIAN_CTRL_LE_DATA_BE);
	hbg_reg_write_field(priv, HBG_REG_MODE_CHANGE_EN_ADDR,
			    HBG_REG_MODE_CHANGE_EN_B, HBG_STATUS_ENABLE);

	hbg_hw_init_rx_control(priv);
	hbg_hw_init_transmit_ctrl(priv);
	return 0;
}
