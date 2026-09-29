// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

/* EDMA v1: initialisation and teardown, hardware start and stop, interrupt
 * setup and the rest of the entry points of the EDMA API. The rings and the
 * datapath are in cfg_rx.c, cfg_tx.c, rx.c and tx.c.
 */

#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/reset.h>

#include "edma.h"
#include "cfg_rx.h"
#include "cfg_tx.h"
#include "../ppe_config.h"

static void edmav1_irq_disable_all(struct edmav1 *priv)
{
	const struct edmav1_soc_data *soc = priv->soc;
	int i;

	for (i = 0; i <= soc->txdesc_ring; i++)
		edma_write(priv->edma,
			   EDMAV1_REG_TX_INT_MASK(soc->tx_int_base, i),
			     0);

	for (i = 0; i <= soc->rxfill_ring; i++)
		edma_write(priv->edma, EDMAV1_REG_RXFILL_INT_MASK(i), 0);

	for (i = 0; i <= soc->rxdesc_ring; i++) {
		edma_write(priv->edma, EDMAV1_REG_RXDESC_INT_MASK(i), 0);
		edma_write(priv->edma, EDMAV1_REG_RX_INT_CTRL(i), 0);
	}
}

static irqreturn_t edmav1_misc_irq_handle(int irq, void *ctx)
{
	struct edmav1 *priv = ctx;
	u32 val;

	edma_read(priv->edma, EDMAV1_REG_MISC_INT_STAT, &val);

	if (!val)
		return IRQ_NONE;

	priv->stats.misc_error++;
	dev_warn_ratelimited(priv->edma->dev, "misc error %#x\n", val);

	return IRQ_HANDLED;
}

int edmav1_ring_alloc(struct edmav1 *priv, struct edmav1_ring *ring, int count,
		      int desc_size)
{
	struct device *dev = priv->edma->dev;

	ring->count = count;
	ring->desc = dma_alloc_coherent(dev, count * desc_size, &ring->dma,
					GFP_KERNEL);
	if (!ring->desc)
		return -ENOMEM;

	return 0;
}

void edmav1_ring_free(struct edmav1 *priv, struct edmav1_ring *ring,
		      int desc_size)
{
	if (ring->desc) {
		dma_free_coherent(priv->edma->dev, ring->count * desc_size,
				  ring->desc, ring->dma);
		ring->desc = NULL;
	}
}

void edmav1_hw_stop(struct edmav1 *priv)
{
	edmav1_irq_disable_all(priv);
	edmav1_rx_rings_disable(priv);
	edmav1_tx_rings_disable(priv);
	edma_write(priv->edma, EDMAV1_REG_PORT_CTRL, 0);
}

static void edmav1_hw_reset(struct edmav1 *priv)
{
	reset_control_assert(priv->edma->rst);
	usleep_range(100, 200);
	reset_control_deassert(priv->edma->rst);
	usleep_range(100, 200);
}

int edmav1_hw_init(struct edmav1 *priv)
{
	const struct edmav1_soc_data *soc = priv->soc;
	int i, ret;
	u32 val;

	edmav1_hw_reset(priv);
	edmav1_hw_stop(priv);

	/* The table packs 8 queue ids per 32-bit entry, 4 bits each. Every
	 * queue id defaults to the mandatory ring, so that a queue id that is
	 * never assigned still delivers somewhere valid. Queue ids
	 * 0..num_rx_queues-1 are in entry 0 and go to the ring of the queue
	 * with that number. The PPE must spread flows over the same number of
	 * queues.
	 */
	val = (soc->rxdesc_ring & EDMAV1_QID2RID_RING_MASK) * 0x11111111u;
	for (i = 0; i < EDMAV1_QID2RID_DEPTH; i++)
		edma_write(priv->edma, EDMAV1_QID2RID_TABLE_MEM(i), val);

	if (priv->num_rx_queues > 1) {
		u32 qid2rid = val;

		for (i = 0; i < priv->num_rx_queues; i++) {
			qid2rid &= ~(EDMAV1_QID2RID_RING_MASK << (i * 4));
			qid2rid |= (priv->rxq[i].ring_index & EDMAV1_QID2RID_RING_MASK)
				   << (i * 4);
		}
		edma_write(priv->edma, EDMAV1_QID2RID_TABLE_MEM(0), qid2rid);

		ret = ppe_rx_hash_spread(priv->edma->ppe_dev,
					 priv->num_rx_queues);
		if (ret)
			return ret;
	}

	ret = edmav1_tx_rings_alloc(priv);
	if (ret)
		return ret;

	ret = edmav1_rx_rings_alloc(priv);
	if (ret) {
		edmav1_tx_rings_free_all(priv);
		return ret;
	}

	edmav1_configure_tx_rings(priv);
	edmav1_configure_rx_rings(priv);

	/* Each rxdesc ring maps 1:1 to its own queue's independent rxfill
	 * ring.
	 */
	edma_write(priv->edma, EDMAV1_REG_RXDESC2FILL_MAP_0, 0);
	edma_write(priv->edma, EDMAV1_REG_RXDESC2FILL_MAP_1, 0);
	for (i = 0; i < priv->num_rx_queues; i++) {
		u8 ring = priv->rxq[i].ring_index;
		u8 fill_ring = priv->rxq[i].rxfill_ring_index;
		u32 reg = ring < 10 ? EDMAV1_REG_RXDESC2FILL_MAP_0 :
				      EDMAV1_REG_RXDESC2FILL_MAP_1;

		edma_set_bits(priv->edma, reg,
			      (fill_ring & 0x7) << ((ring % 10) * 3));
	}

	/* Some SoCs have fewer txcmpl rings than txdesc rings, and a table
	 * tells the completion ring of each txdesc ring. It has 3 bits for each
	 * txdesc ring, in 10 rings for each register.
	 */
	if (soc->txdesc2cmpl_map) {
		for (i = 0; i < 3; i++)
			edma_write(priv->edma, EDMAV1_REG_TXDESC2CMPL_MAP_0 + i * 4, 0);

		for (i = 0; i < priv->num_tx_queues; i++) {
			u8 ring = priv->txq[i].ring_index;

			edma_set_bits(priv->edma,
				      EDMAV1_REG_TXDESC2CMPL_MAP_0 + (ring / 10) * 4,
				      (priv->txq[i].cmpl_ring_index & 0x7) <<
				      ((ring % 10) * 3));
		}
	}

	val = EDMAV1_DMAR_BURST_LEN_SET(soc->burst_enable) |
	      EDMAV1_DMAR_REQ_PRI_SET(0) | EDMAV1_DMAR_TXDATA_NUM_SET(31) |
	      EDMAV1_DMAR_TXDESC_NUM_SET(7) | EDMAV1_DMAR_RXFILL_NUM_SET(7);
	edma_write(priv->edma, EDMAV1_REG_DMAR_CTRL, val);

	if (soc->axiw_enable)
		edma_set_bits(priv->edma, EDMAV1_REG_AXIW_CTRL,
			      EDMAV1_AXIW_MAX_WR_SIZE_EN);

	edma_write(priv->edma, EDMAV1_REG_MISC_INT_MASK, soc->misc_int_mask);

	edma_write(priv->edma, EDMAV1_REG_PORT_CTRL,
		   EDMAV1_PORT_PAD_EN | EDMAV1_PORT_EDMA_EN);

	edmav1_rx_rings_enable(priv);
	edmav1_tx_rings_enable(priv);

	return 0;
}

/* The interrupts are found by name, not by position, so that a device tree
 * that lists them in another order still binds each to its ring.
 *
 * Queue 0, for both RX and TX, is mandatory. The other queues are used only
 * if their interrupt exists, and the search stops at the first ring without
 * one. The request names contain "edma_rxdesc" and "edma_txcmpl" because the
 * packet scheduler finds the rings by searching /proc/interrupts for them.
 */
static int edmav1_irq_init(struct edmav1 *priv)
{
	const struct edmav1_soc_data *soc = priv->soc;
	struct edma *edma = priv->edma;
	int ret, i;

	priv->rxfill_irq = edma_irq_get(edma, "rxfill_%u", soc->rxfill_ring);
	if (priv->rxfill_irq < 0)
		return priv->rxfill_irq;

	priv->misc_irq = edma_irq_get(edma, "misc");
	if (priv->misc_irq < 0)
		return priv->misc_irq;

	priv->rxq[0].priv = priv;
	priv->rxq[0].ring_index = soc->rxdesc_ring;
	priv->rxq[0].rxfill_ring_index = soc->rxfill_ring;
	priv->rxq[0].irq = edma_irq_get(edma, "rxdesc_%u", soc->rxdesc_ring);
	if (priv->rxq[0].irq < 0)
		return priv->rxq[0].irq;

	priv->num_rx_queues = 1;
	for (i = 1; i < EDMAV1_NUM_RX_QUEUES_MAX; i++) {
		u8 ring = soc->rxdesc_ring - i;
		int irq;

		irq = edma_irq_get(edma, "rxdesc_%u", ring);
		if (irq < 0)
			break;

		priv->rxq[i].priv = priv;
		priv->rxq[i].ring_index = ring;
		priv->rxq[i].rxfill_ring_index = soc->rxfill_ring - i;
		priv->rxq[i].irq = irq;
		priv->num_rx_queues++;
	}

	priv->txq[0].priv = priv;
	priv->txq[0].ring_index = soc->txdesc_ring;
	priv->txq[0].cmpl_ring_index = soc->txcmpl_ring;
	priv->txq[0].irq = edma_irq_get(edma, "txcmpl_%u", soc->txcmpl_ring);
	if (priv->txq[0].irq < 0)
		return priv->txq[0].irq;

	priv->num_tx_queues = 1;
	for (i = 1; i < EDMAV1_NUM_TX_QUEUES_MAX; i++) {
		u8 ring = soc->txdesc_ring - i;
		u8 cmpl_ring = soc->txcmpl_ring - i;
		int irq;

		irq = edma_irq_get(edma, "txcmpl_%u", cmpl_ring);
		if (irq < 0)
			break;

		priv->txq[i].priv = priv;
		priv->txq[i].ring_index = ring;
		priv->txq[i].cmpl_ring_index = cmpl_ring;
		priv->txq[i].irq = irq;
		priv->num_tx_queues++;
	}

	ret = edma_irq_request(edma, priv->rxfill_irq, edmav1_rxfill_irq_handle,
			       priv, "edma_rxfill");
	if (ret)
		return ret;

	for (i = 0; i < priv->num_rx_queues; i++) {
		ret = edma_irq_request(edma, priv->rxq[i].irq,
				       edmav1_rxdesc_irq_handle, &priv->rxq[i],
				       "edma_rxdesc_%u", priv->rxq[i].ring_index);
		if (ret)
			return ret;
	}

	for (i = 0; i < priv->num_tx_queues; i++) {
		ret = edma_irq_request(edma, priv->txq[i].irq,
				       edmav1_tx_irq_handle, &priv->txq[i],
				       "edma_txcmpl_%u", priv->txq[i].cmpl_ring_index);
		if (ret)
			return ret;
	}

	return edma_irq_request(edma, priv->misc_irq, edmav1_misc_irq_handle,
				priv, "edma_misc");
}

/* The buffers of a frame size the rings were built for. The rings must not
 * carry frames while this runs.
 */
static int edmav1_rebuild(struct edmav1 *priv, u8 order)
{
	int ret;

	priv->rx_page_order = order;
	priv->rx_buffer_size = edmav1_rx_buffer_size(order);

	ret = edmav1_page_pools_create(priv, order);
	if (ret)
		return ret;

	ret = edmav1_hw_init(priv);
	if (ret)
		edmav1_page_pools_destroy(priv);

	return ret;
}

int edmav1_set_max_frame(struct edma *edma, unsigned int frame_size)
{
	struct edmav1 *priv = edma->priv;
	u8 new_order = edmav1_rx_page_order(frame_size);
	u8 old_order = priv->rx_page_order;
	int ret, restore;

	if (new_order == old_order)
		return 0;

	/* The pages go back to the pool that they came from, so the rings
	 * are emptied before the pools are replaced.
	 */
	edmav1_hw_stop(priv);
	edmav1_rx_rings_drain(priv);
	edmav1_tx_rings_drain(priv);
	edmav1_page_pools_destroy(priv);

	ret = edmav1_rebuild(priv, new_order);
	if (!ret)
		return 0;

	dev_err(edma->dev, "failed to resize the receive buffers: %d\n", ret);

	restore = edmav1_rebuild(priv, old_order);
	if (restore) {
		dev_err(edma->dev, "failed to restore the receive buffers: %d\n",
			restore);
		return restore;
	}

	return ret;
}

void edmav1_open(struct edma *edma)
{
	struct edmav1 *priv = edma->priv;
	int i;

	for (i = 0; i < priv->num_tx_queues; i++)
		napi_enable(&priv->txq[i].napi);

	for (i = 0; i < priv->num_rx_queues; i++)
		napi_enable(&priv->rxq[i].napi);

	edmav1_tx_irq_unmask_all(priv);
	edmav1_rx_irq_unmask_all(priv);
}

void edmav1_close(struct edma *edma)
{
	struct edmav1 *priv = edma->priv;
	int i;

	edmav1_tx_irq_mask_all(priv);
	edmav1_rx_irq_mask_all(priv);

	for (i = 0; i < priv->num_tx_queues; i++)
		napi_disable(&priv->txq[i].napi);

	for (i = 0; i < priv->num_rx_queues; i++)
		napi_disable(&priv->rxq[i].napi);
}

int edmav1_init(struct edma *edma)
{
	const struct edmav1_soc_data *soc = edma->soc_data;
	struct device *dev = edma->dev;
	struct edmav1 *priv;
	int ret, i;

	if (!soc)
		return -EINVAL;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->edma = edma;
	priv->soc = soc;
	for (i = 0; i < EDMAV1_NUM_TX_QUEUES_MAX; i++)
		spin_lock_init(&priv->txq[i].tx_lock);

	/* Finds the number of queues, which the rings depend on. */
	ret = edmav1_irq_init(priv);
	if (ret)
		return ret;

	priv->rx_page_order = edmav1_rx_page_order(EDMAV1_DEFAULT_FRAME_SIZE);
	priv->rx_buffer_size = edmav1_rx_buffer_size(priv->rx_page_order);
	ret = edmav1_page_pools_create(priv, priv->rx_page_order);
	if (ret)
		return ret;

	ret = edmav1_hw_init(priv);
	if (ret)
		goto err_page_pool;

	for (i = 0; i < priv->num_tx_queues; i++)
		netif_napi_add_tx(edma->dummy, &priv->txq[i].napi,
				  edmav1_tx_napi);

	for (i = 0; i < priv->num_rx_queues; i++)
		netif_napi_add(edma->dummy, &priv->rxq[i].napi, edmav1_rx_napi);

	edma->priv = priv;
	edma->caps = (struct edma_caps) {
		.rx_queues = priv->num_rx_queues,
		.tx_queues = priv->num_tx_queues,
		.needed_headroom = EDMAV1_TX_PREHDR_SIZE,
		.min_tx_frame = soc->tx_min_size,
		.max_tx_segs = EDMAV1_TX_MAX_SEGS,
		.max_mtu = EDMAV1_MAX_MTU,
		.features = NETIF_F_SG | NETIF_F_TSO | NETIF_F_TSO6 |
			    NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM |
			    NETIF_F_RXHASH,
	};

	dev_info(dev, "EDMA v1: %d RX and %d TX queues\n",
		 priv->num_rx_queues, priv->num_tx_queues);

	return 0;

err_page_pool:
	edmav1_page_pools_destroy(priv);
	return ret;
}

void edmav1_fini(struct edma *edma)
{
	struct edmav1 *priv = edma->priv;
	int i;

	edmav1_hw_stop(priv);
	edmav1_rx_rings_drain(priv);
	edmav1_tx_rings_drain(priv);
	edmav1_page_pools_destroy(priv);

	for (i = 0; i < priv->num_tx_queues; i++)
		netif_napi_del(&priv->txq[i].napi);

	for (i = 0; i < priv->num_rx_queues; i++)
		netif_napi_del(&priv->rxq[i].napi);
}

static const struct edma_stat_desc edmav1_stat_descs[] = {
	{ .name = "rx_untracked_page", .group = EDMA_STAT_GLOBAL },
	{ .name = "rx_bad_src_info", .group = EDMA_STAT_GLOBAL },
	{ .name = "rx_no_skb", .group = EDMA_STAT_GLOBAL },
	{ .name = "rx_split_frame", .group = EDMA_STAT_GLOBAL },
	{ .name = "rx_fill_starved", .group = EDMA_STAT_GLOBAL },
	{ .name = "tx_desc_error", .group = EDMA_STAT_GLOBAL },
	{ .name = "tx_unnamed_frame", .group = EDMA_STAT_GLOBAL },
	{ .name = "misc_error", .group = EDMA_STAT_GLOBAL },
};

const struct edma_stat_desc *edmav1_stats_layout(struct edma *edma,
						 unsigned int *count)
{
	*count = ARRAY_SIZE(edmav1_stat_descs);

	return edmav1_stat_descs;
}

void edmav1_stats_read(struct edma *edma, u64 *buf)
{
	struct edmav1 *priv = edma->priv;

	BUILD_BUG_ON(sizeof(priv->stats) !=
		     ARRAY_SIZE(edmav1_stat_descs) * sizeof(u64));

	memcpy(buf, &priv->stats, sizeof(priv->stats));
}

int edmav1_ringparam_get(struct edma *edma, struct ethtool_ringparam *rp)
{
	rp->tx_max_pending = EDMAV1_TX_RING_SIZE;
	rp->rx_max_pending = EDMAV1_RX_RING_SIZE;
	rp->tx_pending = EDMAV1_TX_RING_SIZE;
	rp->rx_pending = EDMAV1_RX_RING_SIZE;

	return 0;
}

int edmav1_regs_len(struct edma *edma)
{
	return EDMAV1_REGS_COUNT * 2 * sizeof(u32);
}

/* The registers that the driver sets, in the order that it sets them: the
 * global block, then each ring of the first queue and its interrupt. Each
 * value comes with its offset. The misc status is left out so that a dump
 * cannot take an error away from the handler.
 */
void edmav1_regs_dump(struct edma *edma, void *buf)
{
	struct edmav1 *priv = edma->priv;
	const struct edmav1_soc_data *soc = priv->soc;
	u8 rxdesc_ring = priv->rxq[0].ring_index;
	u8 rxfill_ring = priv->rxq[0].rxfill_ring_index;
	u8 txring = priv->txq[0].ring_index;
	u8 txcmpl_ring = priv->txq[0].cmpl_ring_index;
	const u32 off[EDMAV1_REGS_COUNT] = {
		EDMAV1_REG_PORT_CTRL,
		EDMAV1_REG_DMAR_CTRL,
		EDMAV1_REG_AXIW_CTRL,
		EDMAV1_REG_MISC_INT_MASK,

		EDMAV1_REG_TXDESC_BA(txring),
		EDMAV1_REG_TXDESC_PROD_IDX(txring),
		EDMAV1_REG_TXDESC_CONS_IDX(txring),
		EDMAV1_REG_TXDESC_RING_SIZE(txring),
		EDMAV1_REG_TXDESC_CTRL(txring),

		EDMAV1_REG_TXCMPL_BA(soc->txcmpl_base, txcmpl_ring),
		EDMAV1_REG_TXCMPL_PROD_IDX(soc->txcmpl_base, txcmpl_ring),
		EDMAV1_REG_TXCMPL_CONS_IDX(soc->txcmpl_base, txcmpl_ring),
		EDMAV1_REG_TXCMPL_RING_SIZE(soc->txcmpl_base, txcmpl_ring),
		EDMAV1_REG_TXCMPL_CTRL(soc->txcmpl_base, txcmpl_ring),

		EDMAV1_REG_TX_INT_STAT(soc->tx_int_base, txcmpl_ring),
		EDMAV1_REG_TX_INT_MASK(soc->tx_int_base, txcmpl_ring),
		EDMAV1_REG_TX_MOD_TIMER(soc->tx_int_base, txcmpl_ring),
		EDMAV1_REG_TX_INT_CTRL(soc->tx_int_base, txcmpl_ring),

		EDMAV1_REG_RXFILL_BA(rxfill_ring),
		EDMAV1_REG_RXFILL_PROD_IDX(rxfill_ring),
		EDMAV1_REG_RXFILL_CONS_IDX(rxfill_ring),
		EDMAV1_REG_RXFILL_RING_SIZE(rxfill_ring),
		EDMAV1_REG_RXFILL_RING_EN(rxfill_ring),
		EDMAV1_REG_RXFILL_INT_STAT(rxfill_ring),
		EDMAV1_REG_RXFILL_INT_MASK(rxfill_ring),

		EDMAV1_REG_RXDESC_BA(rxdesc_ring),
		EDMAV1_REG_RXDESC_PROD_IDX(rxdesc_ring),
		EDMAV1_REG_RXDESC_CONS_IDX(rxdesc_ring),
		EDMAV1_REG_RXDESC_RING_SIZE(rxdesc_ring),
		EDMAV1_REG_RXDESC_CTRL(rxdesc_ring),
		EDMAV1_REG_RXDESC_INT_STAT(rxdesc_ring),
		EDMAV1_REG_RXDESC_INT_MASK(rxdesc_ring),
		EDMAV1_REG_RX_MOD_TIMER(rxdesc_ring),
		EDMAV1_REG_RX_INT_CTRL(rxdesc_ring),
	};
	u32 *out = buf;
	int i;

	for (i = 0; i < EDMAV1_REGS_COUNT; i++) {
		u32 val;

		edma_read(edma, off[i], &val);
		*out++ = off[i];
		*out++ = val;
	}
}
