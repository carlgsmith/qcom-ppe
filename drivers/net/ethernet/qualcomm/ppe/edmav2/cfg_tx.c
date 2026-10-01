// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

/* Configure rings, Buffers and NAPI for transmit path along with
 * providing APIs to enable, disable, clean and map the Tx rings.
 */

#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/regmap.h>
#include <linux/skbuff.h>

#include "../ppe.h"
#include "cfg_tx.h"
#include "edma.h"
#include "regs.h"

static void edmav2_cfg_txcmpl_ring_cleanup(struct edmav2_txcmpl_ring *txcmpl_ring)
{
	struct edmav2 *priv = txcmpl_ring->priv;
	struct device *dev = priv->ppe_dev->dev;

	/* Free Tx cmpl ring descriptors. */
	dma_free_coherent(dev, sizeof(struct edmav2_txcmpl_desc)
			  * txcmpl_ring->count, txcmpl_ring->desc,
			  txcmpl_ring->dma);
	txcmpl_ring->desc = NULL;
	txcmpl_ring->dma = (dma_addr_t)0;
}

static int edmav2_cfg_txcmpl_ring_setup(struct edmav2_txcmpl_ring *txcmpl_ring)
{
	struct edmav2 *priv = txcmpl_ring->priv;
	struct device *dev = priv->ppe_dev->dev;

	/* Allocate Tx completion ring descriptors. */
	txcmpl_ring->desc = dma_alloc_coherent(dev, sizeof(struct edmav2_txcmpl_desc)
					       * txcmpl_ring->count,
					       &txcmpl_ring->dma,
					       GFP_KERNEL | __GFP_ZERO);

	if (unlikely(!txcmpl_ring->desc))
		return -ENOMEM;

	return 0;
}

/* Free the frames that are still on the ring. The hardware is stopped. */
static void edmav2_cfg_tx_desc_ring_cleanup(struct edmav2_txdesc_ring *txdesc_ring)
{
	struct edmav2 *priv = txdesc_ring->priv;
	struct device *dev = priv->ppe_dev->dev;
	u32 idx;

	for (idx = 0; idx < txdesc_ring->count; idx++) {
		struct edmav2_txdesc_pri *txdesc;
		struct edmav2_tx_buf *buf = &txdesc_ring->bufs[idx];
		struct sk_buff *skb;

		if (!buf->len)
			continue;

		if (buf->page)
			dma_unmap_page(dev, buf->dma, buf->len, DMA_TO_DEVICE);
		else
			dma_unmap_single(dev, buf->dma, buf->len, DMA_TO_DEVICE);

		/* Only the last descriptor of a frame has the skb. */
		txdesc = EDMAV2_TXDESC_PRI_DESC(txdesc_ring, idx);
		skb = (struct sk_buff *)EDMAV2_TXDESC_OPAQUE_GET(txdesc);
		if (skb)
			dev_kfree_skb_any(skb);
	}

	kfree(txdesc_ring->bufs);
	txdesc_ring->bufs = NULL;

	/* Free Tx ring descriptors. */
	dma_free_coherent(dev, (sizeof(struct edmav2_txdesc_pri)
			  * txdesc_ring->count),
			  txdesc_ring->pdesc,
			  txdesc_ring->pdma);
	txdesc_ring->pdesc = NULL;
	txdesc_ring->pdma = (dma_addr_t)0;

	/* Free any buffers assigned to any secondary descriptors. */
	dma_free_coherent(dev, (sizeof(struct edmav2_txdesc_sec)
			  * txdesc_ring->count),
			  txdesc_ring->sdesc,
			  txdesc_ring->sdma);
	txdesc_ring->sdesc = NULL;
	txdesc_ring->sdma = (dma_addr_t)0;
}

static int edmav2_cfg_tx_desc_ring_setup(struct edmav2_txdesc_ring *txdesc_ring)
{
	struct edmav2 *priv = txdesc_ring->priv;
	struct device *dev = priv->ppe_dev->dev;

	txdesc_ring->bufs = kcalloc(txdesc_ring->count, sizeof(*txdesc_ring->bufs),
				    GFP_KERNEL);
	if (!txdesc_ring->bufs)
		return -ENOMEM;

	txdesc_ring->pdesc = dma_alloc_coherent(dev, sizeof(struct edmav2_txdesc_pri)
						* txdesc_ring->count,
						&txdesc_ring->pdma,
						GFP_KERNEL | __GFP_ZERO);
	if (unlikely(!txdesc_ring->pdesc))
		goto err_bufs;

	txdesc_ring->sdesc = dma_alloc_coherent(dev, sizeof(struct edmav2_txdesc_sec)
						* txdesc_ring->count,
						&txdesc_ring->sdma,
						GFP_KERNEL | __GFP_ZERO);
	if (unlikely(!txdesc_ring->sdesc))
		goto err_pdesc;

	return 0;

err_pdesc:
	dma_free_coherent(dev, (sizeof(struct edmav2_txdesc_pri)
			  * txdesc_ring->count),
			  txdesc_ring->pdesc,
			  txdesc_ring->pdma);
	txdesc_ring->pdesc = NULL;
	txdesc_ring->pdma = (dma_addr_t)0;
err_bufs:
	kfree(txdesc_ring->bufs);
	txdesc_ring->bufs = NULL;

	return -ENOMEM;
}

static void edmav2_cfg_tx_desc_ring_configure(struct edmav2_txdesc_ring *txdesc_ring)
{
	struct edmav2 *priv = txdesc_ring->priv;
	struct ppe_device *ppe_dev = priv->ppe_dev;
	u32 idx_mask = priv->hw_info->idx_mask;
	struct regmap *regmap = ppe_dev->regmap;
	u32 data, reg;

	reg = EDMA_BASE_OFFSET + EDMAV2_REG_TXDESC_BA(txdesc_ring->id);
	regmap_write(regmap, reg, (u32)(txdesc_ring->pdma & EDMAV2_RING_DMA_MASK));

	reg = EDMA_BASE_OFFSET + EDMAV2_REG_TXDESC_BA2(txdesc_ring->id);
	regmap_write(regmap, reg, (u32)(txdesc_ring->sdma & EDMAV2_RING_DMA_MASK));

	reg = EDMA_BASE_OFFSET + EDMAV2_REG_TXDESC_RING_SIZE(txdesc_ring->id);

	regmap_write(regmap, reg, (u32)(txdesc_ring->count & idx_mask));

	/* The consumer index is owned by the hardware, so start empty at its value. */
	regmap_read(regmap, EDMA_BASE_OFFSET + EDMAV2_REG_TXDESC_CONS_IDX(txdesc_ring->id),
		    &data);
	data &= idx_mask;
	txdesc_ring->prod_idx = data;
	reg = EDMA_BASE_OFFSET + EDMAV2_REG_TXDESC_PROD_IDX(txdesc_ring->id);
	regmap_write(regmap, reg, data);

	/* Configure group ID for flow control for this Tx ring. */
	data = field_prep(priv->hw_info->txdesc_fc_grp_id_mask, txdesc_ring->fc_grp_id);
	reg = EDMA_BASE_OFFSET + EDMAV2_REG_TXDESC_CTRL(txdesc_ring->id);
	regmap_write(regmap, reg, data);
}

static void edmav2_cfg_txcmpl_ring_configure(struct edmav2_txcmpl_ring *txcmpl_ring)
{
	struct edmav2 *priv = txcmpl_ring->priv;
	struct ppe_device *ppe_dev = priv->ppe_dev;
	u32 idx_mask = priv->hw_info->idx_mask;
	struct regmap *regmap = ppe_dev->regmap;
	u32 data, reg;

	/* Configure Tx cmpl ring base address. */
	reg = EDMA_BASE_OFFSET + EDMAV2_REG_TXCMPL_BA(txcmpl_ring->id);
	regmap_write(regmap, reg, (u32)(txcmpl_ring->dma & EDMAV2_RING_DMA_MASK));

	reg = EDMA_BASE_OFFSET + EDMAV2_REG_TXCMPL_RING_SIZE(txcmpl_ring->id);
	regmap_write(regmap, reg, (u32)(txcmpl_ring->count & idx_mask));

	/* The producer index is owned by the hardware, so start empty at its value. */
	regmap_read(regmap, EDMA_BASE_OFFSET + EDMAV2_REG_TXCMPL_PROD_IDX(txcmpl_ring->id),
		    &data);
	data &= idx_mask;
	txcmpl_ring->cons_idx = data;
	regmap_write(regmap, EDMA_BASE_OFFSET + EDMAV2_REG_TXCMPL_CONS_IDX(txcmpl_ring->id),
		     data);

	/* Set Tx cmpl ret mode to opaque. */
	reg = EDMA_BASE_OFFSET + EDMAV2_REG_TXCMPL_CTRL(txcmpl_ring->id);
	regmap_write(regmap, reg, EDMAV2_TXCMPL_RETMODE_OPAQUE);

	/* Configure the Mitigation timer. */
	data = EDMAV2_MICROSEC_TO_TIMER_UNIT(EDMAV2_TX_MITIGATION_TIMER_DEF,
					     ppe_dev->clk_rate / MHZ);
	data = ((data & EDMAV2_TX_MOD_TIMER_INIT_MASK)
		<< EDMAV2_TX_MOD_TIMER_INIT_SHIFT);
	reg = EDMA_BASE_OFFSET + EDMAV2_REG_TX_MOD_TIMER(txcmpl_ring->id);
	regmap_write(regmap, reg, data);

	/* Configure the Mitigation packet count. */
	data = (EDMAV2_TX_MITIGATION_PKT_CNT_DEF & EDMAV2_TXCMPL_LOW_THRE_MASK)
		<< EDMAV2_TXCMPL_LOW_THRE_SHIFT;
	reg = EDMA_BASE_OFFSET + EDMAV2_REG_TXCMPL_UGT_THRE(txcmpl_ring->id);
	regmap_write(regmap, reg, data);

	reg = EDMA_BASE_OFFSET + EDMAV2_REG_TX_INT_CTRL(txcmpl_ring->id);
	regmap_write(regmap, reg, EDMAV2_TX_NE_INT_EN);
}

/**
 * edmav2_cfg_tx_rings_enable - Enable Tx rings.
 * @priv: EDMA
 *
 * Enable Tx rings.
 */
void edmav2_cfg_tx_rings_enable(struct edmav2 *priv)
{
	struct regmap *regmap = priv->ppe_dev->regmap;
	u32 i, reg, data;

	for (i = 0; i < priv->hw_info->tx->num_rings; i++) {
		reg = EDMA_BASE_OFFSET + EDMAV2_REG_TXDESC_CTRL(priv->tx_rings[i].id);
		regmap_read(regmap, reg, &data);
		data |= FIELD_PREP(EDMAV2_TXDESC_CTRL_TXEN_MASK, EDMAV2_TXDESC_TX_ENABLE);
		regmap_write(regmap, reg, data);
	}
}

/**
 * edmav2_cfg_tx_rings_disable - Disable Tx rings.
 * @priv: EDMA
 *
 * Disable Tx rings.
 */
void edmav2_cfg_tx_rings_disable(struct edmav2 *priv)
{
	struct regmap *regmap = priv->ppe_dev->regmap;
	u32 i, reg, data;

	for (i = 0; i < priv->hw_info->tx->num_rings; i++) {
		reg = EDMA_BASE_OFFSET + EDMAV2_REG_TXDESC_CTRL(priv->tx_rings[i].id);
		regmap_read(regmap, reg, &data);
		data &= ~EDMAV2_TXDESC_CTRL_TXEN_MASK;
		regmap_write(regmap, reg, data);
	}
}

/* The register that maps the Tx descriptor rings @ring to the Tx completion
 * rings. Each register holds the mapping of 6 rings with 5 bits each.
 */
static u32 edmav2_cfg_tx_map_reg(u32 ring)
{
	static const u32 map_regs[] = {
		EDMAV2_REG_TXDESC2CMPL_MAP_0_ADDR,
		EDMAV2_REG_TXDESC2CMPL_MAP_1_ADDR,
		EDMAV2_REG_TXDESC2CMPL_MAP_2_ADDR,
		EDMAV2_REG_TXDESC2CMPL_MAP_3_ADDR,
		EDMAV2_REG_TXDESC2CMPL_MAP_4_ADDR,
		EDMAV2_REG_TXDESC2CMPL_MAP_5_ADDR,
	};

	return EDMA_BASE_OFFSET + map_regs[ring / 6];
}

/**
 * edmav2_cfg_tx_ring_mappings - Map Tx to Tx complete rings.
 * @priv: EDMA
 *
 * The Tx descriptor ring with the number n uses the Tx completion ring with
 * the number n. The Tx completion ring numbers are counted from
 * the first completion ring.
 */
void edmav2_cfg_tx_ring_mappings(struct edmav2 *priv)
{
	const struct edmav2_hw_info *hw_info = priv->hw_info;
	const struct edmav2_ring_info *txcmpl = hw_info->txcmpl;
	struct regmap *regmap = priv->ppe_dev->regmap;
	const struct edmav2_ring_info *tx = hw_info->tx;
	u32 desc_index, i, data, reg;

	/* Clear the TXDESC2CMPL_MAP_xx reg before setting up
	 * the mapping.
	 */
	for (i = 0; i < 6; i++)
		regmap_write(regmap, edmav2_cfg_tx_map_reg(i * 6), 0);

	desc_index = txcmpl->ring_start;

	for (i = tx->ring_start; i < tx->ring_start + tx->num_rings; i++) {
		reg = edmav2_cfg_tx_map_reg(i);

		regmap_read(regmap, reg, &data);
		data |= (desc_index & EDMAV2_TXDESC2CMPL_MAP_TXDESC_MASK) << ((i % 6) * 5);
		regmap_write(regmap, reg, data);

		desc_index++;
		if (desc_index == txcmpl->ring_start + txcmpl->num_rings)
			desc_index = txcmpl->ring_start;
	}
}

static int edmav2_cfg_tx_rings_setup(struct edmav2 *priv)
{
	const struct edmav2_hw_info *hw_info = priv->hw_info;
	const struct edmav2_ring_info *txcmpl = hw_info->txcmpl;
	const struct edmav2_ring_info *tx = hw_info->tx;
	u32 i;
	int ret;

	for (i = 0; i < tx->num_rings; i++) {
		struct edmav2_txdesc_ring *txdesc_ring = &priv->tx_rings[i];

		txdesc_ring->priv = priv;
		txdesc_ring->count = EDMAV2_TX_RING_SIZE;
		txdesc_ring->id = tx->ring_start + i;

		/* The rings are in the order of the ports, and of the CPUs
		 * for each port. The flow control group is the port number.
		 */
		txdesc_ring->fc_grp_id = i / num_possible_cpus() + 1;
	}

	for (i = 0; i < txcmpl->num_rings; i++) {
		struct edmav2_txcmpl_ring *txcmpl_ring = &priv->txcmpl_rings[i];

		txcmpl_ring->priv = priv;
		txcmpl_ring->count = EDMAV2_TX_RING_SIZE;
		txcmpl_ring->id = txcmpl->ring_start + i;
	}

	/* Allocate TxDesc ring descriptors. */
	for (i = 0; i < tx->num_rings; i++) {
		ret = edmav2_cfg_tx_desc_ring_setup(&priv->tx_rings[i]);
		if (ret) {
			dev_err(priv->ppe_dev->dev,
				"Error in setting up %d txdesc ring. ret: %d\n",
				priv->tx_rings[i].id, ret);
			goto err_tx;
		}
	}

	/* Allocate Tx cmpl ring descriptors. */
	for (i = 0; i < txcmpl->num_rings; i++) {
		ret = edmav2_cfg_txcmpl_ring_setup(&priv->txcmpl_rings[i]);
		if (ret) {
			dev_err(priv->ppe_dev->dev,
				"Error in setting up %d Tx cmpl ring. ret: %d\n",
				priv->txcmpl_rings[i].id, ret);
			goto err_txcmpl;
		}
	}

	return 0;

err_txcmpl:
	while (i--)
		edmav2_cfg_txcmpl_ring_cleanup(&priv->txcmpl_rings[i]);
	i = tx->num_rings;
err_tx:
	while (i--)
		edmav2_cfg_tx_desc_ring_cleanup(&priv->tx_rings[i]);

	return ret;
}

/**
 * edmav2_cfg_tx_rings_alloc - Allocate EDMA Tx rings.
 * @priv: EDMA
 *
 * Allocate EDMA Tx rings.
 */
int edmav2_cfg_tx_rings_alloc(struct edmav2 *priv)
{
	const struct edmav2_hw_info *hw_info = priv->hw_info;
	const struct edmav2_ring_info *txcmpl = hw_info->txcmpl;
	const struct edmav2_ring_info *tx = hw_info->tx;
	int ret;

	priv->tx_rings = kcalloc(tx->num_rings, sizeof(*priv->tx_rings),
				 GFP_KERNEL);
	if (!priv->tx_rings)
		return -ENOMEM;

	priv->txcmpl_rings = kcalloc(txcmpl->num_rings,
				     sizeof(*priv->txcmpl_rings), GFP_KERNEL);
	if (!priv->txcmpl_rings) {
		ret = -ENOMEM;
		goto err_tx_rings;
	}

	ret = edmav2_cfg_tx_rings_setup(priv);
	if (ret)
		goto err_txcmpl_rings;

	return 0;

err_txcmpl_rings:
	kfree(priv->txcmpl_rings);
	priv->txcmpl_rings = NULL;
err_tx_rings:
	kfree(priv->tx_rings);
	priv->tx_rings = NULL;

	return ret;
}

/**
 * edmav2_cfg_tx_rings_cleanup - Cleanup EDMA Tx rings.
 * @priv: EDMA
 *
 * Cleanup EDMA Tx rings.
 */
void edmav2_cfg_tx_rings_cleanup(struct edmav2 *priv)
{
	const struct edmav2_hw_info *hw_info = priv->hw_info;
	u32 i;

	/* Free any buffers assigned to any descriptors. */
	for (i = 0; i < hw_info->tx->num_rings; i++)
		edmav2_cfg_tx_desc_ring_cleanup(&priv->tx_rings[i]);

	/* Free Tx completion descriptors. */
	for (i = 0; i < hw_info->txcmpl->num_rings; i++)
		edmav2_cfg_txcmpl_ring_cleanup(&priv->txcmpl_rings[i]);

	kfree(priv->tx_rings);
	kfree(priv->txcmpl_rings);
	priv->tx_rings = NULL;
	priv->txcmpl_rings = NULL;
}

/**
 * edmav2_cfg_tx_rings - Configure EDMA Tx rings.
 * @priv: EDMA
 *
 * Configure EDMA Tx rings.
 */
void edmav2_cfg_tx_rings(struct edmav2 *priv)
{
	const struct edmav2_hw_info *hw_info = priv->hw_info;
	u32 i;

	/* Configure Tx desc ring. */
	for (i = 0; i < hw_info->tx->num_rings; i++)
		edmav2_cfg_tx_desc_ring_configure(&priv->tx_rings[i]);

	/* Configure Tx cmpl ring. */
	for (i = 0; i < hw_info->txcmpl->num_rings; i++)
		edmav2_cfg_txcmpl_ring_configure(&priv->txcmpl_rings[i]);
}

/**
 * edmav2_cfg_tx_disable_interrupts - EDMA disable TX interrupts.
 * @priv: EDMA
 *
 * Disable TX interrupt masks.
 */
void edmav2_cfg_tx_disable_interrupts(struct edmav2 *priv)
{
	struct regmap *regmap = priv->ppe_dev->regmap;
	u32 i, reg;

	for (i = 0; i < priv->hw_info->txcmpl->num_rings; i++) {
		reg = EDMA_BASE_OFFSET + EDMAV2_REG_TX_INT_MASK(priv->txcmpl_rings[i].id);
		regmap_write(regmap, reg, EDMAV2_MASK_INT_CLEAR);
	}
}

/**
 * edmav2_cfg_tx_enable_interrupts - EDMA enable TX interrupts.
 * @priv: EDMA
 *
 * Enable TX interrupt masks.
 */
void edmav2_cfg_tx_enable_interrupts(struct edmav2 *priv)
{
	struct regmap *regmap = priv->ppe_dev->regmap;
	u32 i, reg;

	for (i = 0; i < priv->hw_info->txcmpl->num_rings; i++) {
		reg = EDMA_BASE_OFFSET + EDMAV2_REG_TX_INT_MASK(priv->txcmpl_rings[i].id);
		regmap_write(regmap, reg, priv->intr_info.intr_mask_txcmpl);
	}
}

/**
 * edmav2_cfg_tx_napi_enable - Enable Tx NAPI.
 * @priv: EDMA
 *
 * Enable Tx NAPI.
 */
void edmav2_cfg_tx_napi_enable(struct edmav2 *priv)
{
	u32 i;

	for (i = 0; i < priv->hw_info->txcmpl->num_rings; i++)
		napi_enable(&priv->txcmpl_rings[i].napi);
}

/**
 * edmav2_cfg_tx_napi_disable - Disable Tx NAPI.
 * @priv: EDMA
 *
 * Disable Tx NAPI.
 */
void edmav2_cfg_tx_napi_disable(struct edmav2 *priv)
{
	u32 i;

	for (i = 0; i < priv->hw_info->txcmpl->num_rings; i++)
		napi_disable(&priv->txcmpl_rings[i].napi);
}

/**
 * edmav2_cfg_tx_napi_delete - Delete Tx NAPI.
 * @priv: EDMA
 *
 * Delete Tx NAPI.
 */
void edmav2_cfg_tx_napi_delete(struct edmav2 *priv)
{
	u32 i;

	for (i = 0; i < priv->hw_info->txcmpl->num_rings; i++)
		netif_napi_del(&priv->txcmpl_rings[i].napi);
}

/**
 * edmav2_cfg_tx_napi_add - TX NAPI add.
 * @priv: EDMA
 *
 * The Tx completion NAPI instances belong to the dummy netdev of the EDMA.
 * The frames that a ring completes belong to the netdev of the port that
 * uses the ring.
 */
void edmav2_cfg_tx_napi_add(struct edmav2 *priv)
{
	u32 i;

	for (i = 0; i < priv->hw_info->txcmpl->num_rings; i++)
		netif_napi_add_weight(priv->edma->dummy,
				      &priv->txcmpl_rings[i].napi,
				      edmav2_tx_napi_poll,
				      priv->hw_info->napi_budget_tx);
}
