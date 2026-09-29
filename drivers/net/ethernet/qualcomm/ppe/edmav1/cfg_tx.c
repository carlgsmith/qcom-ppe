// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

/* TX ring alloc/free, txdesc/txcmpl ring config, TX IRQ mask/unmask. */

#include <linux/regmap.h>

#include "edma.h"
#include "cfg_tx.h"

void edmav1_tx_irq_mask_one(struct edmav1 *priv, u8 ring)
{
	edma_write(priv->edma,
		   EDMAV1_REG_TX_INT_MASK(priv->soc->tx_int_base, ring), 0);
}

void edmav1_tx_irq_unmask_one(struct edmav1 *priv, u8 ring)
{
	edma_write(priv->edma,
		   EDMAV1_REG_TX_INT_MASK(priv->soc->tx_int_base, ring),
		     EDMAV1_TX_INT_MASK);
}

void edmav1_tx_irq_mask_all(struct edmav1 *priv)
{
	int i;

	for (i = 0; i < priv->num_tx_queues; i++)
		edmav1_tx_irq_mask_one(priv, priv->txq[i].cmpl_ring_index);
}

void edmav1_tx_irq_unmask_all(struct edmav1 *priv)
{
	int i;

	for (i = 0; i < priv->num_tx_queues; i++)
		edmav1_tx_irq_unmask_one(priv, priv->txq[i].cmpl_ring_index);
}

int edmav1_tx_ring_alloc(struct edmav1 *priv, struct edmav1_ring *ring,
			 int count, int desc_size)
{
	int ret;

	ret = edmav1_ring_alloc(priv, ring, count, desc_size);
	if (ret)
		return ret;

	ring->skb_store = kcalloc(count, sizeof(struct sk_buff *), GFP_KERNEL);
	if (!ring->skb_store) {
		dma_free_coherent(priv->edma->dev, count * desc_size,
				  ring->desc, ring->dma);
		ring->desc = NULL;
		return -ENOMEM;
	}

	return 0;
}

void edmav1_tx_ring_free(struct edmav1_tx_queue *txq, struct edmav1_ring *ring,
			 int desc_size)
{
	int i;

	if (ring->skb_store) {
		for (i = 0; i < ring->count; i++) {
			struct edmav1_txdesc *txdesc = EDMAV1_TXDESC_DESC(ring, i);
			struct sk_buff *skb = ring->skb_store[i];

			if (skb && (txdesc->word1 & EDMAV1_TXDESC_PREHEADER))
				edmav1_tx_release(txq, i, skb, 0);
		}
		kfree(ring->skb_store);
		ring->skb_store = NULL;
	}

	edmav1_ring_free(txq->priv, ring, desc_size);
}

void edmav1_configure_txdesc_ring(struct edmav1_tx_queue *txq)
{
	u32 val;

	edma_write(txq->priv->edma, EDMAV1_REG_TXDESC_BA(txq->ring_index),
		   (u32)txq->txdesc_ring.dma);

	edma_write(txq->priv->edma,
		   EDMAV1_REG_TXDESC_RING_SIZE(txq->ring_index),
		     txq->txdesc_ring.count & EDMAV1_TXDESC_RING_SIZE_MASK);

	edma_read(txq->priv->edma, EDMAV1_REG_TXDESC_CONS_IDX(txq->ring_index),
		  &val);
	val &= ~EDMAV1_TXDESC_CONS_IDX_MASK;

	edma_update_bits(txq->priv->edma,
			 EDMAV1_REG_TXDESC_PROD_IDX(txq->ring_index),
			   EDMAV1_TXDESC_PROD_IDX_MASK, val);
}

void edmav1_configure_txcmpl_ring(struct edmav1_tx_queue *txq)
{
	const struct edmav1_soc_data *soc = txq->priv->soc;
	u8 ring = txq->cmpl_ring_index;

	edma_write(txq->priv->edma, EDMAV1_REG_TXCMPL_BA(soc->txcmpl_base, ring),
		   (u32)txq->txcmpl_ring.dma);
	edma_write(txq->priv->edma,
		   EDMAV1_REG_TXCMPL_RING_SIZE(soc->txcmpl_base, ring),
		     txq->txcmpl_ring.count & EDMAV1_TXDESC_RING_SIZE_MASK);

	edma_write(txq->priv->edma, EDMAV1_REG_TXCMPL_CTRL(soc->txcmpl_base, ring),
		   EDMAV1_TXCMPL_RETMODE_OPAQUE);

	edma_write(txq->priv->edma, EDMAV1_REG_TX_MOD_TIMER(soc->tx_int_base, ring),
		   EDMAV1_TX_MOD_TIMER);

	edma_write(txq->priv->edma, EDMAV1_REG_TX_INT_CTRL(soc->tx_int_base, ring),
		   0x2);
}

int edmav1_tx_rings_alloc(struct edmav1 *priv)
{
	int ret, i, j;

	for (i = 0; i < priv->num_tx_queues; i++) {
		ret = edmav1_tx_ring_alloc(priv, &priv->txq[i].txdesc_ring,
					   EDMAV1_TX_RING_SIZE,
					 sizeof(struct edmav1_txdesc));
		if (ret)
			goto err_tx;

		ret = edmav1_ring_alloc(priv, &priv->txq[i].txcmpl_ring,
					EDMAV1_TX_RING_SIZE,
				      sizeof(struct edmav1_txcmpl));
		if (ret) {
			edmav1_tx_ring_free(&priv->txq[i], &priv->txq[i].txdesc_ring,
					    sizeof(struct edmav1_txdesc));
			goto err_tx;
		}
	}

	return 0;

err_tx:
	for (j = 0; j < i; j++) {
		edmav1_ring_free(priv, &priv->txq[j].txcmpl_ring,
				 sizeof(struct edmav1_txcmpl));
		edmav1_tx_ring_free(&priv->txq[j], &priv->txq[j].txdesc_ring,
				    sizeof(struct edmav1_txdesc));
	}
	return ret;
}

/* Frees every tx ring, unconditionally - used both by edmav1_tx_rings_drain()
 * and by edmav1_rx_rings_alloc()'s caller when an already-fully-succeeded TX
 * allocation has to be unwound because the RX half that ran after it failed.
 */
void edmav1_tx_rings_free_all(struct edmav1 *priv)
{
	int i;

	for (i = 0; i < priv->num_tx_queues; i++) {
		edmav1_ring_free(priv, &priv->txq[i].txcmpl_ring,
				 sizeof(struct edmav1_txcmpl));
		edmav1_tx_ring_free(&priv->txq[i], &priv->txq[i].txdesc_ring,
				    sizeof(struct edmav1_txdesc));
	}
}

void edmav1_tx_rings_drain(struct edmav1 *priv)
{
	int i;

	for (i = 0; i < priv->num_tx_queues; i++) {
		struct edmav1_tx_queue *txq = &priv->txq[i];

		edmav1_clean_tx(txq, INT_MAX, 0);
		txq->txcmpl_skb = NULL;
		txq->txcmpl_run = false;

		edmav1_tx_ring_free(txq, &txq->txdesc_ring, sizeof(struct edmav1_txdesc));
		edmav1_ring_free(priv, &txq->txcmpl_ring, sizeof(struct edmav1_txcmpl));
	}
}

void edmav1_configure_tx_rings(struct edmav1 *priv)
{
	int i;

	for (i = 0; i < priv->num_tx_queues; i++) {
		edmav1_configure_txdesc_ring(&priv->txq[i]);
		edmav1_configure_txcmpl_ring(&priv->txq[i]);
	}
}

void edmav1_tx_rings_disable(struct edmav1 *priv)
{
	const struct edmav1_soc_data *soc = priv->soc;
	int i;

	for (i = 0; i <= soc->txdesc_ring; i++)
		edma_clear_bits(priv->edma, EDMAV1_REG_TXDESC_CTRL(i),
				EDMAV1_TXDESC_TX_EN);
}

void edmav1_tx_rings_enable(struct edmav1 *priv)
{
	int i;

	for (i = 0; i < priv->num_tx_queues; i++)
		edma_set_bits(priv->edma,
			      EDMAV1_REG_TXDESC_CTRL(priv->txq[i].ring_index),
				EDMAV1_TXDESC_TX_EN);
}
