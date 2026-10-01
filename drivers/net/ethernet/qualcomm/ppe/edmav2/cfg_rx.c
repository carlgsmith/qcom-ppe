// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

/* Configure rings, Buffers and NAPI for receive path along with
 * providing APIs to enable, disable, clean and map the Rx rings.
 */

#include <linux/cpumask.h>
#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/regmap.h>
#include <linux/skbuff.h>

#include "../ppe.h"
#include "../ppe_config.h"
#include "cfg_rx.h"
#include "edma.h"
#include "regs.h"

/* Rx ring queue offset. */
#define EDMAV2_QUEUE_OFFSET(q_id)	((q_id) / EDMAV2_MAX_PRI_PER_CORE)

/* Rx EDMA maximum queue supported. */
#define EDMAV2_CPU_PORT_QUEUE_MAX(queue_start)	\
			((queue_start) + (EDMAV2_MAX_PRI_PER_CORE * num_possible_cpus()) - 1)

/* EDMA Queue ID to Ring ID configuration. */
#define EDMAV2_QID2RID_NUM_PER_REG	4

/* EDMA Queue ID to Ring ID Table. */
#define EDMAV2_QID2RID_TABLE_MEM(q)	(0xb9000 + (0x4 * (q)))

static int rx_queues[] = {0, 8, 16, 24};

static u32 edmav2_rx_ring_queue_map[][EDMAV2_MAX_CORE] = {{ 0, 8, 16, 24 },
						{ 1, 9, 17, 25 },
						{ 2, 10, 18, 26 },
						{ 3, 11, 19, 27 },
						{ 4, 12, 20, 28 },
						{ 5, 13, 21, 29 },
						{ 6, 14, 22, 30 },
						{ 7, 15, 23, 31 }};

/* Map the PPE queues to a Rx descriptor ring. A frame that is queued to one
 * of the queues is delivered to the ring.
 */
static int edmav2_cfg_rx_ring_to_queues(struct ppe_device *ppe_dev, int ring_id,
					int num, const int *queues)
{
	u32 queue_bmap[PPE_RING_TO_QUEUE_BITMAP_WORD_CNT] = {};
	int i;

	for (i = 0; i < num; i++)
		queue_bmap[queues[i] / 32] |= BIT(queues[i] % 32);

	return ppe_ring_queue_map_set(ppe_dev, ring_id, queue_bmap);
}

static int edmav2_cfg_rx_desc_rings_reset_queue_mapping(struct edmav2 *priv)
{
	const struct edmav2_hw_info *hw_info = priv->hw_info;
	const struct edmav2_ring_info *rx = hw_info->rx;
	u32 i, ret;

	for (i = 0; i < rx->num_rings; i++) {
		struct edmav2_rxdesc_ring *rxdesc_ring;

		rxdesc_ring = &priv->rx_rings[i];

		ret = edmav2_cfg_rx_ring_to_queues(priv->ppe_dev, rxdesc_ring->ring_id,
						   ARRAY_SIZE(rx_queues), rx_queues);
		if (ret) {
			pr_err("Error in unmapping rxdesc ring %d to PPE queue mapping to disable its backpressure configuration\n",
			       i);
			return ret;
		}
	}

	return 0;
}

static int edmav2_cfg_rx_desc_ring_reset_queue_priority(struct edmav2 *priv, u32 rxdesc_ring_idx)
{
	u32 i, queue_id, ret;

	for (i = 0; i < EDMAV2_MAX_PRI_PER_CORE; i++) {
		queue_id = edmav2_rx_ring_queue_map[i][rxdesc_ring_idx];

		ret = ppe_queue_priority_set(priv->ppe_dev, queue_id, i);
		if (ret) {
			pr_err("Error in resetting %u queue's priority\n",
			       queue_id);
			return ret;
		}
	}

	return 0;
}

static int edmav2_cfg_rx_desc_ring_reset_queue_config(struct edmav2 *priv)
{
	const struct edmav2_hw_info *hw_info = priv->hw_info;
	const struct edmav2_ring_info *rx = hw_info->rx;
	u32 i, ret;

	if (unlikely(rx->num_rings > num_possible_cpus())) {
		pr_err("Invalid count of rxdesc rings: %d\n",
		       rx->num_rings);
		return -EINVAL;
	}

	/* Unmap Rxdesc ring to PPE queue mapping */
	ret = edmav2_cfg_rx_desc_rings_reset_queue_mapping(priv);
	if (ret)	{
		pr_err("Error in resetting Rx desc ring backpressure config\n");
		return ret;
	}

	/* Reset the priority for PPE queues mapped to Rx rings */
	for (i = 0; i < rx->num_rings; i++) {
		ret =  edmav2_cfg_rx_desc_ring_reset_queue_priority(priv, i);
		if (ret)	{
			pr_err("Error in resetting ring:%d queue's priority\n",
			       i + rx->ring_start);
			return ret;
		}
	}

	return 0;
}

static int edmav2_cfg_rx_desc_ring_to_queue_mapping(struct edmav2 *priv)
{
	const struct edmav2_hw_info *hw_info = priv->hw_info;
	const struct edmav2_ring_info *rx = hw_info->rx;
	u32 i;
	int ret;

	/* Rxdesc ring to PPE queue mapping */
	for (i = 0; i < rx->num_rings; i++) {
		struct edmav2_rxdesc_ring *rxdesc_ring;

		rxdesc_ring = &priv->rx_rings[i];

		ret = edmav2_cfg_rx_ring_to_queues(priv->ppe_dev,
						   rxdesc_ring->ring_id,
						   ARRAY_SIZE(rx_queues),
						   rx_queues);
		if (ret) {
			pr_err("Error in configuring Rx ring to PPE queue mapping, ret: %d, id: %d\n",
			       ret, rxdesc_ring->ring_id);
			if (!edmav2_cfg_rx_desc_rings_reset_queue_mapping(priv))
				pr_err("Error in resetting Rx desc ring configurations\n");

			return ret;
		}

		pr_debug("Rx desc ring %d to PPE queue mapping for backpressure:\n",
			 rxdesc_ring->ring_id);
	}

	return 0;
}

static void edmav2_cfg_rx_desc_ring_configure(struct edmav2_rxdesc_ring *rxdesc_ring)
{
	struct edmav2 *priv = rxdesc_ring->priv;
	struct ppe_device *ppe_dev = priv->ppe_dev;
	u32 idx_mask = priv->hw_info->idx_mask;
	struct regmap *regmap = ppe_dev->regmap;
	u32 data, reg;

	reg = EDMA_BASE_OFFSET + EDMAV2_REG_RXDESC_BA(rxdesc_ring->ring_id);
	regmap_write(regmap, reg, (u32)(rxdesc_ring->pdma & EDMAV2_RXDESC_BA_MASK));

	reg = EDMA_BASE_OFFSET + EDMAV2_REG_RXDESC_PREHEADER_BA(rxdesc_ring->ring_id);
	regmap_write(regmap, reg, (u32)(rxdesc_ring->sdma & EDMAV2_RXDESC_PREHEADER_BA_MASK));

	/* The producer index is owned by the hardware, so start empty at its value. */
	regmap_read(regmap, EDMA_BASE_OFFSET + EDMAV2_REG_RXDESC_PROD_IDX(rxdesc_ring->ring_id),
		    &data);
	data &= idx_mask;
	rxdesc_ring->cons_idx = data;
	regmap_write(regmap, EDMA_BASE_OFFSET + EDMAV2_REG_RXDESC_CONS_IDX(rxdesc_ring->ring_id),
		     data);

	data = rxdesc_ring->count & idx_mask;

	/* Some SoCs have no PL offset field in the Rxdesc ring size register. */
	if (!priv->hw_info->rxdesc_no_pl_offset)
		data |= (EDMAV2_RXDESC_PL_DEFAULT_VALUE & EDMAV2_RXDESC_PL_OFFSET_MASK)
			 << EDMAV2_RXDESC_PL_OFFSET_SHIFT;

	reg = EDMA_BASE_OFFSET + EDMAV2_REG_RXDESC_RING_SIZE(rxdesc_ring->ring_id);
	regmap_write(regmap, reg, data);

	/* Configure the Mitigation timer */
	data = EDMAV2_MICROSEC_TO_TIMER_UNIT(EDMAV2_RX_MITIGATION_TIMER_DEF,
					     ppe_dev->clk_rate / MHZ);
	data = ((data & EDMAV2_RX_MOD_TIMER_INIT_MASK)
			<< EDMAV2_RX_MOD_TIMER_INIT_SHIFT);
	reg = EDMA_BASE_OFFSET + EDMAV2_REG_RX_MOD_TIMER(rxdesc_ring->ring_id);
	regmap_write(regmap, reg, data);

	/* Configure the Mitigation packet count */
	data = (EDMAV2_RX_MITIGATION_PKT_CNT_DEF & EDMAV2_RXDESC_LOW_THRE_MASK)
			<< EDMAV2_RXDESC_LOW_THRE_SHIFT;
	reg = EDMA_BASE_OFFSET + EDMAV2_REG_RXDESC_UGT_THRE(rxdesc_ring->ring_id);
	regmap_write(regmap, reg, data);

	/* Enable ring. Set ret mode to 'opaque'. */
	reg = EDMA_BASE_OFFSET + EDMAV2_REG_RX_INT_CTRL(rxdesc_ring->ring_id);
	regmap_write(regmap, reg, EDMAV2_RX_NE_INT_EN);
}

static void edmav2_cfg_rx_qid_to_rx_desc_ring_mapping(struct edmav2 *priv)
{
	u32 desc_index, ring_index, reg_index, data, q_id;
	const struct edmav2_hw_info *hw_info = priv->hw_info;
	struct ppe_device *ppe_dev = priv->ppe_dev;
	struct regmap *regmap = ppe_dev->regmap;
	const struct edmav2_ring_info *rx = hw_info->rx;
	u32 mcast_start, mcast_end, reg;
	int ret;

	desc_index = (rx->ring_start & EDMAV2_RX_RING_ID_MASK);

	/* There are 4 Rx desc rings, one for each core.
	 * Map the unicast queues to Rx desc rings.
	 */
	for (q_id = EDMAV2_RX_QUEUE_START;
		q_id <= EDMAV2_CPU_PORT_QUEUE_MAX(EDMAV2_RX_QUEUE_START);
			q_id += EDMAV2_QID2RID_NUM_PER_REG) {
		reg_index = q_id / EDMAV2_QID2RID_NUM_PER_REG;
		ring_index = desc_index + EDMAV2_QUEUE_OFFSET(q_id);

		data = FIELD_PREP(EDMAV2_RX_RING_ID_QUEUE0_MASK, ring_index);
		data |= FIELD_PREP(EDMAV2_RX_RING_ID_QUEUE1_MASK, ring_index);
		data |=	FIELD_PREP(EDMAV2_RX_RING_ID_QUEUE2_MASK, ring_index);
		data |=	FIELD_PREP(EDMAV2_RX_RING_ID_QUEUE3_MASK, ring_index);

		reg = EDMA_BASE_OFFSET + EDMAV2_QID2RID_TABLE_MEM(reg_index);
		regmap_write(regmap, reg, data);
		pr_debug("Configure QID2RID: %d reg:0x%x to 0x%x, desc_index: %d, reg_index: %d\n",
			 q_id, EDMAV2_QID2RID_TABLE_MEM(reg_index), data, desc_index, reg_index);
	}

	ret = ppe_port_resource_get(priv->ppe_dev, 0, PPE_RES_MCAST,
				    &mcast_start, &mcast_end);
	if (ret < 0) {
		pr_err("Error in extracting multicast queue values\n");
		return;
	}

	/* Map multicast queues to the first Rx ring. */
	desc_index = (rx->ring_start & EDMAV2_RX_RING_ID_MASK);
	for (q_id = mcast_start; q_id <= mcast_end;
			q_id += EDMAV2_QID2RID_NUM_PER_REG) {
		reg_index = q_id / EDMAV2_QID2RID_NUM_PER_REG;

		data = FIELD_PREP(EDMAV2_RX_RING_ID_QUEUE0_MASK, desc_index);
		data |=	FIELD_PREP(EDMAV2_RX_RING_ID_QUEUE1_MASK, desc_index);
		data |=	FIELD_PREP(EDMAV2_RX_RING_ID_QUEUE2_MASK, desc_index);
		data |=	FIELD_PREP(EDMAV2_RX_RING_ID_QUEUE3_MASK, desc_index);

		reg = EDMA_BASE_OFFSET + EDMAV2_QID2RID_TABLE_MEM(reg_index);
		regmap_write(regmap, reg, data);

		pr_debug("Configure QID2RID: %d reg:0x%x to 0x%x\n",
			 q_id, EDMAV2_QID2RID_TABLE_MEM(reg_index), data);
	}
}

static void edmav2_cfg_rx_rings_to_rx_fill_mapping(struct edmav2 *priv)
{
	const struct edmav2_hw_info *hw_info = priv->hw_info;
	struct ppe_device *ppe_dev = priv->ppe_dev;
	struct regmap *regmap = ppe_dev->regmap;
	const struct edmav2_ring_info *rx = hw_info->rx;
	u32 i, data, reg;

	/* Set RXDESC2FILL_MAP_xx reg.
	 * 3 registers hold the Rxfill mapping for all Rx desc rings.
	 * 3 bits holds the Rx fill ring mapping for each of the
	 * Rx descriptor ring.
	 */
	regmap_write(regmap, EDMA_BASE_OFFSET + EDMAV2_REG_RXDESC2FILL_MAP_0_ADDR, 0);
	regmap_write(regmap, EDMA_BASE_OFFSET + EDMAV2_REG_RXDESC2FILL_MAP_1_ADDR, 0);
	regmap_write(regmap, EDMA_BASE_OFFSET + EDMAV2_REG_RXDESC2FILL_MAP_2_ADDR, 0);

	for (i = 0; i < rx->num_rings; i++) {
		struct edmav2_rxdesc_ring *rxdesc_ring = &priv->rx_rings[i];
		u32 data, reg, ring_id;

		ring_id = rxdesc_ring->ring_id;
		if (ring_id >= 0 && ring_id <= 9)
			reg = EDMA_BASE_OFFSET + EDMAV2_REG_RXDESC2FILL_MAP_0_ADDR;
		else if (ring_id >= 10 && ring_id <= 19)
			reg = EDMA_BASE_OFFSET + EDMAV2_REG_RXDESC2FILL_MAP_1_ADDR;
		else
			reg = EDMA_BASE_OFFSET + EDMAV2_REG_RXDESC2FILL_MAP_2_ADDR;

		pr_debug("Configure RXDESC:%u to use RXFILL:%u\n",
			 ring_id,
			 rxdesc_ring->rxfill->ring_id);

		 /* Set the Rx fill ring number in the mapping register. */
		regmap_read(regmap, reg, &data);
		data |= (rxdesc_ring->rxfill->ring_id &
			 EDMAV2_RXDESC2FILL_MAP_RXDESC_MASK) <<
			 ((ring_id % 10) * 3);
		regmap_write(regmap, reg, data);
	}

	reg = EDMA_BASE_OFFSET + EDMAV2_REG_RXDESC2FILL_MAP_0_ADDR;
	regmap_read(regmap, reg, &data);
	pr_debug("EDMAV2_REG_RXDESC2FILL_MAP_0_ADDR: 0x%x\n", data);

	reg = EDMA_BASE_OFFSET + EDMAV2_REG_RXDESC2FILL_MAP_1_ADDR;
	regmap_read(regmap, reg, &data);
	pr_debug("EDMAV2_REG_RXDESC2FILL_MAP_1_ADDR: 0x%x\n", data);

	reg = EDMA_BASE_OFFSET + EDMAV2_REG_RXDESC2FILL_MAP_2_ADDR;
	regmap_read(regmap, reg, &data);
	pr_debug("EDMAV2_REG_RXDESC2FILL_MAP_2_ADDR: 0x%x\n", data);
}

/**
 * edmav2_cfg_rx_rings_enable - Enable Rx and Rxfill rings
 *
 * Enable Rx and Rxfill rings.
 */
void edmav2_cfg_rx_rings_enable(struct edmav2 *priv)
{
	const struct edmav2_hw_info *hw_info = priv->hw_info;
	const struct edmav2_ring_info *rxfill = hw_info->rxfill;
	struct ppe_device *ppe_dev = priv->ppe_dev;
	struct regmap *regmap = ppe_dev->regmap;
	const struct edmav2_ring_info *rx = hw_info->rx;
	u32 i, reg;

	for (i = rx->ring_start; i < rx->ring_start + rx->num_rings; i++) {
		u32 data;

		reg = EDMA_BASE_OFFSET + EDMAV2_REG_RXDESC_CTRL(i);
		regmap_read(regmap, reg, &data);
		data |= EDMAV2_RXDESC_RX_EN;
		regmap_write(regmap, reg, data);
		regmap_clear_bits(regmap, EDMA_BASE_OFFSET + EDMAV2_REG_RXDESC_DISABLE(i),
				  EDMAV2_RXDESC_RX_DISABLE);
	}

	for (i = rxfill->ring_start; i < rxfill->ring_start + rxfill->num_rings; i++) {
		u32 data;

		reg = EDMA_BASE_OFFSET + EDMAV2_REG_RXFILL_RING_EN(i);
		regmap_read(regmap, reg, &data);
		data |= EDMAV2_RXFILL_RING_EN;
		regmap_write(regmap, reg, data);
		regmap_clear_bits(regmap, EDMA_BASE_OFFSET + EDMAV2_REG_RXFILL_DISABLE(i),
				  EDMAV2_RXFILL_RING_DISABLE);
	}
}

/* The disable bit makes the hardware finish its transfers and set the done register. */
static void edmav2_cfg_rx_desc_ring_disable(struct ppe_device *ppe_dev, u32 ring_id)
{
	struct regmap *regmap = ppe_dev->regmap;
	u32 data;
	int ret;

	regmap_clear_bits(regmap, EDMA_BASE_OFFSET + EDMAV2_REG_RXDESC_CTRL(ring_id),
			  EDMAV2_RXDESC_RX_EN);
	regmap_set_bits(regmap, EDMA_BASE_OFFSET + EDMAV2_REG_RXDESC_DISABLE(ring_id),
			EDMAV2_RXDESC_RX_DISABLE);
	ret = regmap_read_poll_timeout(regmap,
				       EDMA_BASE_OFFSET + EDMAV2_REG_RXDESC_DISABLE_DONE(ring_id),
				       data, data, 10, 10000);
	if (ret)
		dev_warn(ppe_dev->dev, "Rx ring %u did not stop\n", ring_id);
}

static void edmav2_cfg_rxfill_ring_disable(struct ppe_device *ppe_dev, u32 ring_id)
{
	struct regmap *regmap = ppe_dev->regmap;
	u32 data;
	int ret;

	regmap_clear_bits(regmap, EDMA_BASE_OFFSET + EDMAV2_REG_RXFILL_RING_EN(ring_id),
			  EDMAV2_RXFILL_RING_EN);
	regmap_set_bits(regmap, EDMA_BASE_OFFSET + EDMAV2_REG_RXFILL_DISABLE(ring_id),
			EDMAV2_RXFILL_RING_DISABLE);
	ret = regmap_read_poll_timeout(regmap,
				       EDMA_BASE_OFFSET + EDMAV2_REG_RXFILL_DISABLE_DONE(ring_id),
				       data, data, 10, 10000);
	if (ret)
		dev_warn(ppe_dev->dev, "Rxfill ring %u did not stop\n", ring_id);
}

/**
 * edmav2_cfg_rx_rings_disable - Disable Rx and Rxfill rings
 *
 * Disable Rx and Rxfill rings.
 */
void edmav2_cfg_rx_rings_disable(struct edmav2 *priv)
{
	const struct edmav2_hw_info *hw_info = priv->hw_info;
	u32 i;

	for (i = 0; i < hw_info->rx->num_rings; i++)
		edmav2_cfg_rx_desc_ring_disable(priv->ppe_dev, priv->rx_rings[i].ring_id);

	for (i = 0; i < hw_info->rxfill->num_rings; i++)
		edmav2_cfg_rxfill_ring_disable(priv->ppe_dev, priv->rxfill_rings[i].ring_id);
}

/**
 * edmav2_cfg_rx_rings_disable_all - Disable every Rx and Rxfill ring of the hardware
 */
void edmav2_cfg_rx_rings_disable_all(struct edmav2 *priv)
{
	const struct edmav2_hw_info *hw_info = priv->hw_info;
	u32 i;

	for (i = 0; i < hw_info->rx->max_rings; i++)
		edmav2_cfg_rx_desc_ring_disable(priv->ppe_dev, i);

	for (i = 0; i < hw_info->rxfill->max_rings; i++)
		edmav2_cfg_rxfill_ring_disable(priv->ppe_dev, i);
}

/**
 * edmav2_cfg_rx_mappings - Setup RX ring mapping
 *
 * Setup queue ID to Rx desc ring mapping.
 */
void edmav2_cfg_rx_ring_mappings(struct edmav2 *priv)
{
	edmav2_cfg_rx_qid_to_rx_desc_ring_mapping(priv);
	edmav2_cfg_rx_rings_to_rx_fill_mapping(priv);
}

static void edmav2_cfg_rx_fill_ring_cleanup(struct edmav2_rxfill_ring *rxfill_ring)
{
	struct edmav2 *priv = rxfill_ring->priv;
	struct ppe_device *ppe_dev = priv->ppe_dev;
	u32 idx_mask = priv->hw_info->idx_mask;
	struct regmap *regmap = ppe_dev->regmap;
	struct device *dev = ppe_dev->dev;
	u32 dma_map_size = rxfill_ring->alloc_size - EDMAV2_RX_SKB_HEADROOM - NET_IP_ALIGN;
	u32 cons_idx, curr_idx;
	u32 data, reg;

	/* Get RxFill ring producer index */
	curr_idx = rxfill_ring->prod_idx & idx_mask;

	/* Get RxFill ring consumer index */
	reg = EDMA_BASE_OFFSET + EDMAV2_REG_RXFILL_CONS_IDX(rxfill_ring->ring_id);
	regmap_read(regmap, reg, &data);

	cons_idx = data & idx_mask;

	while (curr_idx != cons_idx) {
		struct edmav2_rxfill_desc *rxfill_desc;
		struct sk_buff *skb;

		/* Get RxFill descriptor */
		rxfill_desc = EDMAV2_RXFILL_DESC(rxfill_ring, cons_idx);

		cons_idx = (cons_idx + 1) & EDMAV2_RX_RING_SIZE_MASK;

		/* Get skb from opaque */
		skb = (struct sk_buff *)EDMAV2_RXFILL_OPAQUE_GET(rxfill_desc);
		if (unlikely(!skb))
			continue;

		dma_unmap_single(dev, le32_to_cpu((__force __le32)rxfill_desc->word0),
				 dma_map_size, DMA_FROM_DEVICE);
		dev_kfree_skb_any(skb);
	}

	/* Free RxFill ring descriptors */
	dma_free_coherent(dev, (sizeof(struct edmav2_rxfill_desc)
			   * rxfill_ring->count),
			   rxfill_ring->desc, rxfill_ring->dma);
	rxfill_ring->desc = NULL;
	rxfill_ring->dma = (dma_addr_t)0;
}

static int edmav2_cfg_rx_fill_ring_dma_alloc(struct edmav2_rxfill_ring *rxfill_ring)
{
	struct edmav2 *priv = rxfill_ring->priv;
	struct ppe_device *ppe_dev = priv->ppe_dev;
	struct device *dev = ppe_dev->dev;

	rxfill_ring->desc = dma_alloc_coherent(dev, (sizeof(struct edmav2_rxfill_desc)
					       * rxfill_ring->count),
					       &rxfill_ring->dma,
					       GFP_KERNEL | __GFP_ZERO);
	if (unlikely(!rxfill_ring->desc))
		return -ENOMEM;

	return 0;
}

static int edmav2_cfg_rx_desc_ring_dma_alloc(struct edmav2_rxdesc_ring *rxdesc_ring)
{
	struct edmav2 *priv = rxdesc_ring->priv;
	struct ppe_device *ppe_dev = priv->ppe_dev;
	struct device *dev = ppe_dev->dev;

	rxdesc_ring->pdesc = dma_alloc_coherent(dev, (sizeof(struct edmav2_rxdesc_pri)
						* rxdesc_ring->count),
				&rxdesc_ring->pdma, GFP_KERNEL | __GFP_ZERO);
	if (unlikely(!rxdesc_ring->pdesc))
		return -ENOMEM;

	rxdesc_ring->sdesc = dma_alloc_coherent(dev, (sizeof(struct edmav2_rxdesc_sec)
				* rxdesc_ring->count),
				&rxdesc_ring->sdma, GFP_KERNEL | __GFP_ZERO);
	if (unlikely(!rxdesc_ring->sdesc)) {
		dma_free_coherent(dev, (sizeof(struct edmav2_rxdesc_pri)
				  * rxdesc_ring->count),
				  rxdesc_ring->pdesc,
				  rxdesc_ring->pdma);
		rxdesc_ring->pdesc = NULL;
		rxdesc_ring->pdma = (dma_addr_t)0;
		return -ENOMEM;
	}

	return 0;
}

static void edmav2_cfg_rx_desc_ring_cleanup(struct edmav2_rxdesc_ring *rxdesc_ring)
{
	struct edmav2 *priv = rxdesc_ring->priv;
	struct ppe_device *ppe_dev = priv->ppe_dev;
	u32 idx_mask = priv->hw_info->idx_mask;
	struct regmap *regmap = ppe_dev->regmap;
	struct device *dev = ppe_dev->dev;
	u32 dma_map_size = rxdesc_ring->rxfill->alloc_size - EDMAV2_RX_SKB_HEADROOM -
			   NET_IP_ALIGN;
	u32 prod_idx, cons_idx, reg;

	/* Get Rxdesc consumer & producer indices */
	cons_idx = rxdesc_ring->cons_idx & idx_mask;

	reg = EDMA_BASE_OFFSET + EDMAV2_REG_RXDESC_PROD_IDX(rxdesc_ring->ring_id);
	regmap_read(regmap, reg, &prod_idx);

	prod_idx = prod_idx & idx_mask;

	/* Free any buffers assigned to any descriptors */
	while (cons_idx != prod_idx) {
		struct edmav2_rxdesc_pri *rxdesc_pri =
			EDMAV2_RXDESC_PRI_DESC(rxdesc_ring, cons_idx);
		struct sk_buff *skb;

		/* Update consumer index */
		cons_idx = (cons_idx + 1) & EDMAV2_RX_RING_SIZE_MASK;

		/* Get opaque from Rxdesc */
		skb = (struct sk_buff *)EDMAV2_RXDESC_OPAQUE_GET(rxdesc_pri);
		if (unlikely(!skb))
			continue;

		dma_unmap_single(dev, EDMAV2_RXDESC_BUFFER_ADDR_GET(rxdesc_pri),
				 dma_map_size, DMA_FROM_DEVICE);
		dev_kfree_skb_any(skb);
	}

	/* Update the consumer index */
	reg = EDMA_BASE_OFFSET + EDMAV2_REG_RXDESC_CONS_IDX(rxdesc_ring->ring_id);
	regmap_write(regmap, reg, cons_idx);

	/* Free Rxdesc ring descriptor */
	dma_free_coherent(dev, (sizeof(struct edmav2_rxdesc_pri)
			  * rxdesc_ring->count), rxdesc_ring->pdesc,
			  rxdesc_ring->pdma);
	rxdesc_ring->pdesc = NULL;
	rxdesc_ring->pdma = (dma_addr_t)0;

	/* Free any buffers assigned to any secondary ring descriptors */
	dma_free_coherent(dev, (sizeof(struct edmav2_rxdesc_sec)
			  * rxdesc_ring->count), rxdesc_ring->sdesc,
			  rxdesc_ring->sdma);
	rxdesc_ring->sdesc = NULL;
	rxdesc_ring->sdma = (dma_addr_t)0;
}

static int edmav2_cfg_rx_rings_setup(struct edmav2 *priv)
{
	const struct edmav2_hw_info *hw_info = priv->hw_info;
	const struct edmav2_ring_info *rxfill = hw_info->rxfill;
	const struct edmav2_ring_info *rx = hw_info->rx;
	u32 ring_idx, alloc_size, buf_len;
	int ret;

	alloc_size = EDMAV2_RX_BUFFER_SIZE;
	buf_len = alloc_size - EDMAV2_RX_SKB_HEADROOM - NET_IP_ALIGN;

	/* Allocate Rx fill ring descriptors */
	for (ring_idx = 0; ring_idx < rxfill->num_rings; ring_idx++) {
		struct edmav2_rxfill_ring *rxfill_ring = &priv->rxfill_rings[ring_idx];

		rxfill_ring->priv = priv;
		rxfill_ring->count = EDMAV2_RX_RING_SIZE;
		rxfill_ring->ring_id = rxfill->ring_start + ring_idx;
		rxfill_ring->alloc_size = alloc_size;
		rxfill_ring->buf_len = buf_len;

		ret = edmav2_cfg_rx_fill_ring_dma_alloc(rxfill_ring);
		if (ret) {
			dev_err(priv->ppe_dev->dev,
				"Error in setting up %d rxfill ring. ret: %d\n",
				rxfill_ring->ring_id, ret);
			goto err_rxfill;
		}
	}

	/* Allocate RxDesc ring descriptors */
	for (ring_idx = 0; ring_idx < rx->num_rings; ring_idx++) {
		struct edmav2_rxdesc_ring *rxdesc_ring = &priv->rx_rings[ring_idx];

		rxdesc_ring->priv = priv;
		rxdesc_ring->count = EDMAV2_RX_RING_SIZE;
		rxdesc_ring->ring_id = rx->ring_start + ring_idx;

		/* Create a mapping between RX Desc ring and Rx fill ring.
		 * Number of fill rings are lesser than the descriptor rings
		 * Share the fill rings across descriptor rings.
		 */
		rxdesc_ring->rxfill = &priv->rxfill_rings[ring_idx % rxfill->num_rings];

		ret = edmav2_cfg_rx_desc_ring_dma_alloc(rxdesc_ring);
		if (ret) {
			dev_err(priv->ppe_dev->dev,
				"Error in setting up %d rxdesc ring. ret: %d\n",
				rxdesc_ring->ring_id, ret);
			goto err_rxdesc;
		}
	}

	return 0;

err_rxdesc:
	while (ring_idx--)
		edmav2_cfg_rx_desc_ring_cleanup(&priv->rx_rings[ring_idx]);
	ring_idx = rxfill->num_rings;
err_rxfill:
	while (ring_idx--)
		edmav2_cfg_rx_fill_ring_cleanup(&priv->rxfill_rings[ring_idx]);

	return ret;
}

static void edmav2_cfg_rx_fill_ring_configure(struct edmav2_rxfill_ring *rxfill_ring)
{
	struct edmav2 *priv = rxfill_ring->priv;
	struct ppe_device *ppe_dev = priv->ppe_dev;
	u32 idx_mask = priv->hw_info->idx_mask;
	struct regmap *regmap = ppe_dev->regmap;
	u32 ring_sz, reg;

	reg = EDMA_BASE_OFFSET + EDMAV2_REG_RXFILL_BA(rxfill_ring->ring_id);
	regmap_write(regmap, reg, (u32)(rxfill_ring->dma & EDMAV2_RING_DMA_MASK));

	ring_sz = rxfill_ring->count & idx_mask;

	if (priv->hw_info->rxfill_size_in_buffer1_reg)
		reg = EDMA_BASE_OFFSET + EDMAV2_REG_RXFILL_BUFFER1_SIZE(rxfill_ring->ring_id);
	else
		reg = EDMA_BASE_OFFSET + EDMAV2_REG_RXFILL_RING_SIZE(rxfill_ring->ring_id);

	regmap_write(regmap, reg, ring_sz);

	/* The consumer index is owned by the hardware, so start empty at its value. */
	regmap_read(regmap, EDMA_BASE_OFFSET + EDMAV2_REG_RXFILL_CONS_IDX(rxfill_ring->ring_id),
		    &reg);
	reg &= idx_mask;
	rxfill_ring->prod_idx = reg;
	regmap_write(regmap, EDMA_BASE_OFFSET + EDMAV2_REG_RXFILL_PROD_IDX(rxfill_ring->ring_id),
		     reg);

	edmav2_rx_alloc_buffer(rxfill_ring, rxfill_ring->count - 1);
}

static void edmav2_cfg_rx_desc_ring_flow_control(struct edmav2 *priv,
						 u32 threshold_xoff,
						 u32 threshold_xon)
{
	const struct edmav2_hw_info *hw_info = priv->hw_info;
	struct ppe_device *ppe_dev = priv->ppe_dev;
	struct regmap *regmap = ppe_dev->regmap;
	const struct edmav2_ring_info *rx = hw_info->rx;
	u32 data, i, reg;

	data = (threshold_xoff & EDMAV2_RXDESC_FC_XOFF_THRE_MASK) <<
	       EDMAV2_RXDESC_FC_XOFF_THRE_SHIFT;
	data |= (threshold_xon & EDMAV2_RXDESC_FC_XON_THRE_MASK) <<
		EDMAV2_RXDESC_FC_XON_THRE_SHIFT;

	for (i = 0; i < rx->num_rings; i++) {
		struct edmav2_rxdesc_ring *rxdesc_ring;

		rxdesc_ring = &priv->rx_rings[i];
		reg = EDMA_BASE_OFFSET + EDMAV2_REG_RXDESC_FC_THRE(rxdesc_ring->ring_id);
		regmap_write(regmap, reg, data);
	}
}

static void edmav2_cfg_rx_fill_ring_flow_control(struct edmav2 *priv,
						 int threshold_xoff,
						 int threshold_xon)
{
	const struct edmav2_hw_info *hw_info = priv->hw_info;
	const struct edmav2_ring_info *rxfill = hw_info->rxfill;
	struct ppe_device *ppe_dev = priv->ppe_dev;
	struct regmap *regmap = ppe_dev->regmap;
	u32 data, i, reg;

	data = (threshold_xoff & EDMAV2_RXFILL_FC_XOFF_THRE_MASK) <<
	       EDMAV2_RXFILL_FC_XOFF_THRE_SHIFT;
	data |= (threshold_xon & EDMAV2_RXFILL_FC_XON_THRE_MASK) <<
		EDMAV2_RXFILL_FC_XON_THRE_SHIFT;

	for (i = 0; i < rxfill->num_rings; i++) {
		struct edmav2_rxfill_ring *rxfill_ring;

		rxfill_ring = &priv->rxfill_rings[i];
		reg = EDMA_BASE_OFFSET + EDMAV2_REG_RXFILL_FC_THRE(rxfill_ring->ring_id);
		regmap_write(regmap, reg, data);
	}
}

/**
 * edmav2_cfg_rx_rings_alloc - Allocate EDMA Rx rings
 * @priv: EDMA
 *
 * Allocate EDMA Rx rings.
 *
 * Return 0 on success, negative error code on failure.
 */
int edmav2_cfg_rx_rings_alloc(struct edmav2 *priv)
{
	const struct edmav2_hw_info *hw_info = priv->hw_info;
	const struct edmav2_ring_info *rxfill = hw_info->rxfill;
	const struct edmav2_ring_info *rx = hw_info->rx;
	int ret;

	priv->rxfill_rings = kcalloc(rxfill->num_rings,
				     sizeof(*priv->rxfill_rings), GFP_KERNEL);
	if (!priv->rxfill_rings)
		return -ENOMEM;

	priv->rx_rings = kcalloc(rx->num_rings, sizeof(*priv->rx_rings),
				 GFP_KERNEL);
	if (!priv->rx_rings) {
		ret = -ENOMEM;
		goto err_rxfill_rings;
	}

	ret = edmav2_cfg_rx_rings_setup(priv);
	if (ret)
		goto err_rx_rings;

	/* Reset Rx descriptor ring mapped queue's configurations */
	ret = edmav2_cfg_rx_desc_ring_reset_queue_config(priv);
	if (ret) {
		edmav2_cfg_rx_rings_cleanup(priv);
		return ret;
	}

	return 0;

err_rx_rings:
	kfree(priv->rx_rings);
	priv->rx_rings = NULL;
err_rxfill_rings:
	kfree(priv->rxfill_rings);
	priv->rxfill_rings = NULL;

	return ret;
}

/**
 * edmav2_cfg_rx_rings_cleanup - Cleanup EDMA Rx rings
 *
 * Cleanup EDMA Rx rings
 */
void edmav2_cfg_rx_rings_cleanup(struct edmav2 *priv)
{
	const struct edmav2_hw_info *hw_info = priv->hw_info;
	const struct edmav2_ring_info *rxfill = hw_info->rxfill;
	const struct edmav2_ring_info *rx = hw_info->rx;
	u32 i;

	/* Free RxFill ring descriptors */
	for (i = 0; i < rxfill->num_rings; i++)
		edmav2_cfg_rx_fill_ring_cleanup(&priv->rxfill_rings[i]);

	/* Free Rx completion ring descriptors */
	for (i = 0; i < rx->num_rings; i++)
		edmav2_cfg_rx_desc_ring_cleanup(&priv->rx_rings[i]);

	kfree(priv->rxfill_rings);
	kfree(priv->rx_rings);
	priv->rxfill_rings = NULL;
	priv->rx_rings = NULL;
}

/**
 * edmav2_cfg_rx_rings - Configure EDMA Rx rings.
 *
 * Configure EDMA Rx rings.
 */
int edmav2_cfg_rx_rings(struct edmav2 *priv)
{
	const struct edmav2_hw_info *hw_info = priv->hw_info;
	const struct edmav2_ring_info *rxfill = hw_info->rxfill;
	const struct edmav2_ring_info *rx = hw_info->rx;
	u32 i;

	for (i = 0; i < rxfill->num_rings; i++)
		edmav2_cfg_rx_fill_ring_configure(&priv->rxfill_rings[i]);

	for (i = 0; i < rx->num_rings; i++)
		edmav2_cfg_rx_desc_ring_configure(&priv->rx_rings[i]);

	/* Configure Rx flow control configurations */
	edmav2_cfg_rx_desc_ring_flow_control(priv, EDMAV2_RX_FC_XOFF_DEF, EDMAV2_RX_FC_XON_DEF);
	edmav2_cfg_rx_fill_ring_flow_control(priv, EDMAV2_RX_FC_XOFF_DEF, EDMAV2_RX_FC_XON_DEF);

	return edmav2_cfg_rx_desc_ring_to_queue_mapping(priv);
}

/**
 * edmav2_cfg_rx_disable_interrupts - EDMA disable RX interrupts
 *
 * Disable RX interrupt masks
 */
void edmav2_cfg_rx_disable_interrupts(struct edmav2 *priv)
{
	const struct edmav2_hw_info *hw_info = priv->hw_info;
	struct ppe_device *ppe_dev = priv->ppe_dev;
	struct regmap *regmap = ppe_dev->regmap;
	const struct edmav2_ring_info *rx = hw_info->rx;
	u32 i, reg;

	for (i = 0; i < rx->num_rings; i++) {
		struct edmav2_rxdesc_ring *rxdesc_ring =
				&priv->rx_rings[i];
		reg = EDMA_BASE_OFFSET + EDMAV2_REG_RXDESC_INT_MASK(rxdesc_ring->ring_id);
		regmap_write(regmap, reg, EDMAV2_MASK_INT_CLEAR);
	}
}

/**
 * edmav2_cfg_rx_enable_interrupts - EDMA enable RX interrupts
 *
 * Enable RX interrupt masks
 */
void edmav2_cfg_rx_enable_interrupts(struct edmav2 *priv)
{
	const struct edmav2_hw_info *hw_info = priv->hw_info;
	struct ppe_device *ppe_dev = priv->ppe_dev;
	struct regmap *regmap = ppe_dev->regmap;
	const struct edmav2_ring_info *rx = hw_info->rx;
	u32 i, reg;

	for (i = 0; i < rx->num_rings; i++) {
		struct edmav2_rxdesc_ring *rxdesc_ring =
				&priv->rx_rings[i];
		reg = EDMA_BASE_OFFSET + EDMAV2_REG_RXDESC_INT_MASK(rxdesc_ring->ring_id);
		regmap_write(regmap, reg, priv->intr_info.intr_mask_rx);
	}
}

/**
 * edmav2_cfg_rx_napi_disable - Disable NAPI for Rx
 * @priv: EDMA
 *
 * Disable NAPI for Rx
 */
void edmav2_cfg_rx_napi_disable(struct edmav2 *priv)
{
	u32 i;

	for (i = 0; i < priv->hw_info->rx->num_rings; i++)
		napi_disable(&priv->rx_rings[i].napi);
}

/**
 * edmav2_cfg_rx_napi_enable - Enable NAPI for Rx
 * @priv: EDMA
 *
 * Enable NAPI for Rx
 */
void edmav2_cfg_rx_napi_enable(struct edmav2 *priv)
{
	u32 i;

	for (i = 0; i < priv->hw_info->rx->num_rings; i++)
		napi_enable(&priv->rx_rings[i].napi);
}

/**
 * edmav2_cfg_rx_napi_delete - Delete Rx NAPI
 * @priv: EDMA
 *
 * Delete RX NAPI
 */
void edmav2_cfg_rx_napi_delete(struct edmav2 *priv)
{
	u32 i;

	for (i = 0; i < priv->hw_info->rx->num_rings; i++)
		netif_napi_del(&priv->rx_rings[i].napi);
}

/* Add Rx NAPI */
/**
 * edmav2_cfg_rx_napi_add - Add Rx NAPI
 * @priv: EDMA
 *
 * The Rx NAPI instances belong to the dummy netdev of the EDMA.
 */
void edmav2_cfg_rx_napi_add(struct edmav2 *priv)
{
	u32 i;

	for (i = 0; i < priv->hw_info->rx->num_rings; i++)
		netif_napi_add_weight(priv->edma->dummy, &priv->rx_rings[i].napi,
				      edmav2_rx_napi_poll,
				      priv->hw_info->napi_budget_rx);
}

/**
 * edmav2_cfg_rx_rps_hash_map - Configure rx rps hash map.
 * @priv: EDMA
 *
 * Initialize and configure RPS hash map for queues
 */
int edmav2_cfg_rx_rps_hash_map(struct edmav2 *priv)
{
	u32 q_map[EDMAV2_MAX_CORE];
	u32 hash, idx = 0, cpu;
	int ret;

	/* Map all possible hash values to queues used by the EDMA Rx
	 * rings, one queue for each core. These queues are mapped to
	 * different Rx rings which are assigned to different cores using
	 * IRQ affinity configuration.
	 */
	for (cpu = 0; cpu < EDMAV2_MAX_CORE; cpu++)
		q_map[cpu] = EDMAV2_RX_QUEUE_START + cpu * EDMAV2_MAX_PRI_PER_CORE;

	for (hash = 0; hash < PPE_QUEUE_HASH_NUM; hash++) {
		ret = ppe_queue_ucast_offset_hash_set(priv->ppe_dev,
						      EDMAV2_CPU_PORT_PROFILE_ID,
						      hash, q_map[idx]);
		if (ret)
			return ret;

		idx = (idx + 1) % EDMAV2_MAX_CORE;
	}

	return 0;
}

