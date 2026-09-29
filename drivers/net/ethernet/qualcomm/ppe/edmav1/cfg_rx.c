// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

/* RX ring alloc/free, rxdesc/rxfill ring config, RX IRQ mask/unmask, page
 * pool lifecycle.
 */

#include <linux/regmap.h>

#include "edma.h"
#include "cfg_rx.h"
#include "rx.h"

void edmav1_rxdesc_irq_mask_one(struct edmav1 *priv, u8 ring)
{
	edma_write(priv->edma, EDMAV1_REG_RXDESC_INT_MASK(ring), 0);
}

void edmav1_rxdesc_irq_unmask_one(struct edmav1 *priv, u8 ring)
{
	edma_write(priv->edma, EDMAV1_REG_RXDESC_INT_MASK(ring),
		   EDMAV1_RXDESC_INT_MASK_PKT_INT);
}

void edmav1_rx_irq_mask_all(struct edmav1 *priv)
{
	const struct edmav1_soc_data *soc = priv->soc;
	int i;

	edma_write(priv->edma, EDMAV1_REG_RXFILL_INT_MASK(soc->rxfill_ring), 0);
	for (i = 0; i < priv->num_rx_queues; i++)
		edmav1_rxdesc_irq_mask_one(priv, priv->rxq[i].ring_index);
}

void edmav1_rx_irq_unmask_all(struct edmav1 *priv)
{
	const struct edmav1_soc_data *soc = priv->soc;
	int i;

	edma_write(priv->edma, EDMAV1_REG_RXFILL_INT_MASK(soc->rxfill_ring),
		   EDMAV1_RXFILL_INT_MASK);
	for (i = 0; i < priv->num_rx_queues; i++)
		edmav1_rxdesc_irq_unmask_one(priv, priv->rxq[i].ring_index);
}

int edmav1_rx_ring_alloc(struct edmav1 *priv, struct edmav1_ring *ring,
			 int count, int desc_size)
{
	int ret;

	ret = edmav1_ring_alloc(priv, ring, count, desc_size);
	if (ret)
		return ret;

	ring->page_store = kcalloc(count, sizeof(*ring->page_store),
				   GFP_KERNEL);
	if (!ring->page_store) {
		dma_free_coherent(priv->edma->dev, count * desc_size,
				  ring->desc, ring->dma);
		ring->desc = NULL;
		return -ENOMEM;
	}

	return 0;
}

void edmav1_rx_ring_free(struct edmav1_rx_queue *rxq, struct edmav1_ring *ring,
			 int desc_size)
{
	int i;

	if (ring->page_store) {
		/* Hardware indices do not account for prefetched RX pages. */
		for (i = 0; i < ring->count; i++) {
			if (!ring->page_store[i])
				continue;

			page_pool_put_full_page(rxq->page_pool,
						ring->page_store[i], false);
			ring->page_store[i] = NULL;
		}

		kfree(ring->page_store);
		ring->page_store = NULL;
	}

	edmav1_ring_free(rxq->priv, ring, desc_size);
}

void edmav1_configure_rxdesc_ring(struct edmav1_rx_queue *rxq)
{
	u32 val;

	edma_write(rxq->priv->edma, EDMAV1_REG_RXDESC_BA(rxq->ring_index),
		   (u32)rxq->rxdesc_ring.dma);

	val = rxq->rxdesc_ring.count & EDMAV1_RXDESC_RING_SIZE_MASK;
	val |= (EDMAV1_RX_PREHDR_SIZE & EDMAV1_RXDESC_PL_OFFSET_MASK)
	       << EDMAV1_RXDESC_PL_OFFSET_SHIFT;
	edma_write(rxq->priv->edma,
		   EDMAV1_REG_RXDESC_RING_SIZE(rxq->ring_index), val);

	edma_write(rxq->priv->edma, EDMAV1_REG_RX_MOD_TIMER(rxq->ring_index),
		   EDMAV1_RX_MOD_TIMER_INIT);

	edma_write(rxq->priv->edma, EDMAV1_REG_RX_INT_CTRL(rxq->ring_index),
		   0x2);
}

void edmav1_configure_rxfill_ring(struct edmav1_rx_queue *rxq)
{
	edma_write(rxq->priv->edma, EDMAV1_REG_RXFILL_BA(rxq->rxfill_ring_index),
		   (u32)rxq->rxfill_ring.dma);

	edma_write(rxq->priv->edma,
		   EDMAV1_REG_RXFILL_RING_SIZE(rxq->rxfill_ring_index),
		     rxq->rxfill_ring.count & EDMAV1_RXFILL_RING_SIZE_MASK);

	edmav1_rx_fill(rxq);
}

u8 edmav1_rx_page_order(unsigned int frame_size)
{
	size_t size = NET_SKB_PAD + EDMAV1_RX_PREHDR_SIZE + frame_size +
		      SKB_DATA_ALIGN(sizeof(struct skb_shared_info));

	return get_order(size);
}

u32 edmav1_rx_buffer_size(u8 order)
{
	return (PAGE_SIZE << order) - NET_SKB_PAD -
	       SKB_DATA_ALIGN(sizeof(struct skb_shared_info));
}

static struct page_pool *edmav1_page_pool_create(struct edmav1 *priv,
						 u8 order)
{
	struct page_pool_params pp = {
		.order     = order,
		.pool_size = EDMAV1_RX_RING_SIZE,
		.nid       = NUMA_NO_NODE,
		.dev       = priv->edma->dev,
		.dma_dir   = DMA_FROM_DEVICE,
		.offset    = NET_SKB_PAD,
		.max_len   = edmav1_rx_buffer_size(order),
		.flags     = PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV,
	};

	return page_pool_create(&pp);
}

void edmav1_page_pools_destroy(struct edmav1 *priv)
{
	int i;

	for (i = 0; i < priv->num_rx_queues; i++) {
		if (priv->rxq[i].page_pool) {
			page_pool_destroy(priv->rxq[i].page_pool);
			priv->rxq[i].page_pool = NULL;
		}
	}
}

int edmav1_page_pools_create(struct edmav1 *priv, u8 order)
{
	int i, ret;

	for (i = 0; i < priv->num_rx_queues; i++) {
		priv->rxq[i].page_pool = edmav1_page_pool_create(priv, order);
		if (IS_ERR(priv->rxq[i].page_pool)) {
			ret = PTR_ERR(priv->rxq[i].page_pool);
			priv->rxq[i].page_pool = NULL;
			goto err;
		}
	}

	return 0;

err:
	for (i--; i >= 0; i--) {
		page_pool_destroy(priv->rxq[i].page_pool);
		priv->rxq[i].page_pool = NULL;
	}
	return ret;
}

int edmav1_rx_rings_alloc(struct edmav1 *priv)
{
	int ret, i, j;

	for (i = 0; i < priv->num_rx_queues; i++) {
		ret = edmav1_rx_ring_alloc(priv, &priv->rxq[i].rxfill_ring,
					   EDMAV1_RX_RING_SIZE,
					 sizeof(struct edmav1_rxfill_desc));
		if (ret)
			goto err_rx;

		ret = edmav1_ring_alloc(priv, &priv->rxq[i].rxdesc_ring,
					EDMAV1_RX_RING_SIZE,
				      sizeof(struct edmav1_rxdesc));
		if (ret) {
			edmav1_rx_ring_free(&priv->rxq[i], &priv->rxq[i].rxfill_ring,
					    sizeof(struct edmav1_rxfill_desc));
			goto err_rx;
		}
	}

	return 0;

err_rx:
	for (j = 0; j < i; j++) {
		edmav1_ring_free(priv, &priv->rxq[j].rxdesc_ring,
				 sizeof(struct edmav1_rxdesc));
		edmav1_rx_ring_free(&priv->rxq[j], &priv->rxq[j].rxfill_ring,
				    sizeof(struct edmav1_rxfill_desc));
	}
	return ret;
}

void edmav1_rx_rings_drain(struct edmav1 *priv)
{
	int i;

	for (i = 0; i < priv->num_rx_queues; i++) {
		edmav1_ring_free(priv, &priv->rxq[i].rxdesc_ring,
				 sizeof(struct edmav1_rxdesc));
		edmav1_rx_ring_free(&priv->rxq[i], &priv->rxq[i].rxfill_ring,
				    sizeof(struct edmav1_rxfill_desc));
	}
}

void edmav1_configure_rx_rings(struct edmav1 *priv)
{
	int i;

	for (i = 0; i < priv->num_rx_queues; i++) {
		edmav1_configure_rxfill_ring(&priv->rxq[i]);
		edmav1_configure_rxdesc_ring(&priv->rxq[i]);
	}
}

void edmav1_rx_rings_disable(struct edmav1 *priv)
{
	const struct edmav1_soc_data *soc = priv->soc;
	int i;

	for (i = 0; i <= soc->rxdesc_ring; i++)
		edma_clear_bits(priv->edma, EDMAV1_REG_RXDESC_CTRL(i),
				EDMAV1_RXDESC_RX_EN);

	for (i = 0; i <= soc->rxfill_ring; i++)
		edma_clear_bits(priv->edma, EDMAV1_REG_RXFILL_RING_EN(i),
				EDMAV1_RXFILL_RING_EN);
}

void edmav1_rx_rings_enable(struct edmav1 *priv)
{
	int i;

	for (i = 0; i < priv->num_rx_queues; i++) {
		edma_set_bits(priv->edma,
			      EDMAV1_REG_RXDESC_CTRL(priv->rxq[i].ring_index),
				EDMAV1_RXDESC_RX_EN);
		edma_set_bits(priv->edma,
			      EDMAV1_REG_RXFILL_RING_EN(priv->rxq[i].rxfill_ring_index),
				EDMAV1_RXFILL_RING_EN);
	}
}
