// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

/* Provide APIs to fill the Tx descriptors and transmit Scatter Gather and
 * linear packets, and Tx complete to free the skb after transmit.
 */

#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/regmap.h>
#include <net/gso.h>

#include "../ppe.h"
#include "../ppe_config.h"
#include "cfg_tx.h"
#include "edma.h"
#include "regs.h"

/* A frame uses one descriptor for its head, one for each fragment and one for
 * each skb of its frag list and the fragments of those.
 */
static u32 edmav2_tx_num_descs_for_sg(struct sk_buff *skb)
{
	u32 num_tx_desc_needed = 1 + skb_shinfo(skb)->nr_frags;

	if (unlikely(skb_has_frag_list(skb))) {
		struct sk_buff *iter_skb;

		skb_walk_frags(skb, iter_skb)
			num_tx_desc_needed += 1 + skb_shinfo(iter_skb)->nr_frags;
	}

	return num_tx_desc_needed;
}

/* The hardware cannot do TSO for a frame with more segments than tso_max, so
 * the frame is segmented in software.
 */
static enum edmav2_tx_gso_status edmav2_tx_gso_segment(struct edmav2 *priv,
						       struct sk_buff *skb,
						       struct net_device *netdev,
						       struct sk_buff **segs)
{
	/* Check is skb is non-linear to proceed. */
	if (likely(!skb_is_nonlinear(skb)))
		return EDMAV2_TX_GSO_NOT_NEEDED;

	if (likely(edmav2_tx_num_descs_for_sg(skb) <= priv->hw_info->tso_max))
		return EDMAV2_TX_GSO_NOT_NEEDED;

	/* GSO segmentation of the skb into multiple segments. */
	*segs = skb_gso_segment(skb, netdev->features
		& ~(NETIF_F_TSO | NETIF_F_TSO6));

	/* Check for error in GSO segmentation. */
	if (IS_ERR_OR_NULL(*segs)) {
		netdev_info(netdev, "Tx gso fail\n");
		return EDMAV2_TX_GSO_FAIL;
	}

	return EDMAV2_TX_GSO_SUCCEED;
}

/* The queue of the netdev that transmits through the Tx descriptor ring with
 * the same index as the Tx completion ring. The rings are in the order of
 * the ports and, for each port, of the CPUs.
 */
static struct netdev_queue *edmav2_tx_ring_queue(struct edmav2 *priv,
						 struct edmav2_txcmpl_ring *txcmpl_ring)
{
	u32 idx = txcmpl_ring - priv->txcmpl_rings;
	u32 ncpu = num_possible_cpus();
	struct net_device *netdev;

	netdev = READ_ONCE(priv->edma->netdev[idx / ncpu + EDMAV2_START_IFNUM]);
	if (!netdev)
		return NULL;

	return netdev_get_tx_queue(netdev, idx % ncpu);
}

static void edmav2_tx_unmap_buf(struct device *dev, struct edmav2_tx_buf *buf)
{
	if (buf->page)
		dma_unmap_page(dev, buf->dma, buf->len, DMA_TO_DEVICE);
	else
		dma_unmap_single(dev, buf->dma, buf->len, DMA_TO_DEVICE);

	buf->len = 0;
}

/**
 * edmav2_tx_complete - Reap Tx completion descriptors.
 * @work_to_do: Work to do.
 * @txcmpl_ring: Tx Completion ring.
 *
 * Reap Tx completion descriptors of the transmitted
 * packets and free the corresponding SKBs.
 *
 * Return the number descriptors for which Tx complete is done.
 */
static u32 edmav2_tx_complete(u32 work_to_do, struct edmav2_txcmpl_ring *txcmpl_ring)
{
	struct edmav2 *priv = txcmpl_ring->priv;
	struct edmav2_txcmpl_stats *txcmpl_stats = &txcmpl_ring->txcmpl_stats;
	struct edmav2_txdesc_ring *txdesc_ring =
		&priv->tx_rings[txcmpl_ring - priv->txcmpl_rings];
	struct ppe_device *ppe_dev = priv->ppe_dev;
	u32 idx_mask = priv->hw_info->idx_mask;
	struct regmap *regmap = ppe_dev->regmap;
	struct device *dev = ppe_dev->dev;
	u32 avail, count, txcmpl_errors;
	struct edmav2_txcmpl_desc *txcmpl;
	struct netdev_queue *nq;
	u32 cons_idx, data;
	struct sk_buff *skb;
	u32 reg;

	cons_idx = txcmpl_ring->cons_idx;

	if (likely(txcmpl_ring->avail_pkt >= work_to_do)) {
		avail = work_to_do;
	} else {
		/* Get Tx cmpl ring producer index. */
		reg = EDMA_BASE_OFFSET + EDMAV2_REG_TXCMPL_PROD_IDX(txcmpl_ring->id);
		regmap_read(regmap, reg, &data);

		data &= idx_mask;

		avail = EDMAV2_DESC_AVAIL_COUNT(data, cons_idx, EDMAV2_TX_RING_SIZE);
		txcmpl_ring->avail_pkt = avail;

		if (unlikely(!avail)) {
			u64_stats_update_begin(&txcmpl_stats->syncp);
			++txcmpl_stats->no_pending_desc;
			u64_stats_update_end(&txcmpl_stats->syncp);
			return 0;
		}

		avail = min(avail, work_to_do);
	}

	count = avail;

	while (likely(avail--)) {
		txcmpl = EDMAV2_TXCMPL_DESC(txcmpl_ring, cons_idx);

		/* The completion ring has one entry for each descriptor, in
		 * the order of the descriptors, so the buffer of the
		 * descriptor is at the same index.
		 */
		if (likely(txdesc_ring->bufs[cons_idx].len))
			edmav2_tx_unmap_buf(dev, &txdesc_ring->bufs[cons_idx]);

		/* The last descriptor of a frame holds the SKB pointer. */
		if (unlikely(EDMAV2_TXCMPL_MORE_BIT_GET(txcmpl))) {
			u64_stats_update_begin(&txcmpl_stats->syncp);
			++txcmpl_stats->desc_with_more_bit;
			u64_stats_update_end(&txcmpl_stats->syncp);
			cons_idx = (cons_idx + 1) & EDMAV2_TX_RING_SIZE_MASK;
			continue;
		}

		skb = (struct sk_buff *)EDMAV2_TXCMPL_OPAQUE_GET(txcmpl);
		if (unlikely(!skb)) {
			dev_warn_ratelimited(dev, "Invalid cons_idx:%u word2:%x word3:%x\n",
					     cons_idx, txcmpl->word2, txcmpl->word3);

			u64_stats_update_begin(&txcmpl_stats->syncp);
			++txcmpl_stats->invalid_buffer;
			u64_stats_update_end(&txcmpl_stats->syncp);
		} else {
			txcmpl_errors = EDMAV2_TXCOMP_RING_ERROR_GET(txcmpl->word3);
			if (unlikely(txcmpl_errors)) {
				dev_err_ratelimited(dev, "Error 0x%0x observed in tx complete %d ring\n",
						    txcmpl_errors, txcmpl_ring->id);

				u64_stats_update_begin(&txcmpl_stats->syncp);
				++txcmpl_stats->errors;
				u64_stats_update_end(&txcmpl_stats->syncp);
			}

			dev_kfree_skb(skb);
		}

		cons_idx = (cons_idx + 1) & EDMAV2_TX_RING_SIZE_MASK;
	}

	txcmpl_ring->cons_idx = cons_idx;
	txcmpl_ring->avail_pkt -= count;

	reg = EDMA_BASE_OFFSET + EDMAV2_REG_TXCMPL_CONS_IDX(txcmpl_ring->id);
	regmap_write(regmap, reg, cons_idx);

	/* A frame that was refused for lack of descriptors stopped the queue of
	 * the port that uses this ring. Only the completion of descriptors
	 * frees space, so the queue is woken here.
	 */
	nq = edmav2_tx_ring_queue(priv, txcmpl_ring);
	if (unlikely(nq && netif_tx_queue_stopped(nq))) {
		__netif_tx_lock(nq, smp_processor_id());
		netif_tx_wake_queue(nq);
		__netif_tx_unlock(nq);
	}

	return count;
}

/**
 * edmav2_tx_napi_poll - EDMA TX NAPI handler.
 * @napi: NAPI structure.
 * @budget: Tx NAPI Budget.
 *
 * EDMA TX NAPI handler.
 */
int edmav2_tx_napi_poll(struct napi_struct *napi, int budget)
{
	struct edmav2_txcmpl_ring *txcmpl_ring =
		container_of(napi, struct edmav2_txcmpl_ring, napi);
	struct edmav2 *priv = txcmpl_ring->priv;
	struct ppe_device *ppe_dev = priv->ppe_dev;
	struct regmap *regmap = ppe_dev->regmap;
	u32 txcmpl_intr_status;
	int work_done = 0;
	u32 data, reg;

	do {
		work_done += edmav2_tx_complete(budget - work_done, txcmpl_ring);
		if (work_done >= budget)
			return work_done;

		reg = EDMA_BASE_OFFSET + EDMAV2_REG_TX_INT_STAT(txcmpl_ring->id);
		regmap_read(regmap, reg, &data);
		txcmpl_intr_status = data & EDMAV2_TXCMPL_RING_INT_STATUS_MASK;
	} while (txcmpl_intr_status);

	/* No more packets to process. Finish NAPI processing. */
	napi_complete(napi);

	/* Set Tx cmpl ring interrupt mask. */
	reg = EDMA_BASE_OFFSET + EDMAV2_REG_TX_INT_MASK(txcmpl_ring->id);
	regmap_write(regmap, reg, priv->intr_info.intr_mask_txcmpl);

	return work_done;
}

/**
 * edmav2_tx_handle_irq - Tx IRQ Handler.
 * @irq: Interrupt request.
 * @ctx: Context.
 *
 * Process TX IRQ and schedule NAPI.
 *
 * Return IRQ handler code.
 */
irqreturn_t edmav2_tx_handle_irq(int irq, void *ctx)
{
	struct edmav2_txcmpl_ring *txcmpl_ring = ctx;
	struct edmav2 *priv = txcmpl_ring->priv;
	struct ppe_device *ppe_dev = priv->ppe_dev;
	struct regmap *regmap = ppe_dev->regmap;
	u32 reg;

	if (likely(napi_schedule_prep(&txcmpl_ring->napi))) {
		/* Disable TxCmpl intr. */
		reg = EDMA_BASE_OFFSET + EDMAV2_REG_TX_INT_MASK(txcmpl_ring->id);
		regmap_write(regmap, reg, EDMAV2_MASK_INT_DISABLE);
		__napi_schedule(&txcmpl_ring->napi);
	}

	return IRQ_HANDLED;
}

/* Map one buffer and remember it, so that the completion can unmap it. */
static bool edmav2_tx_map_buf(struct edmav2_txdesc_ring *txdesc_ring, u32 idx,
			      void *data, skb_frag_t *frag, u32 len)
{
	struct device *dev = txdesc_ring->priv->ppe_dev->dev;
	struct edmav2_tx_buf *buf = &txdesc_ring->bufs[idx];

	if (frag) {
		buf->dma = skb_frag_dma_map(dev, frag, 0, len, DMA_TO_DEVICE);
		buf->page = true;
	} else {
		buf->dma = dma_map_single(dev, data, len, DMA_TO_DEVICE);
		buf->page = false;
	}

	if (dma_mapping_error(dev, buf->dma))
		return false;

	buf->len = len;

	return true;
}

/* Map the head and the fragments of one skb. Zero size segments are skipped.
 * They can occur during TSO when the primary segment holds nothing but the
 * headers, and they make the hardware hang.
 */
static int edmav2_tx_map_one(struct edmav2_txdesc_ring *txdesc_ring,
			     struct sk_buff *skb, u32 *idx)
{
	int i;

	if (skb_headlen(skb)) {
		if (!edmav2_tx_map_buf(txdesc_ring, *idx, skb->data, NULL,
				       skb_headlen(skb)))
			return -ENOMEM;

		*idx = (*idx + 1) & EDMAV2_TX_RING_SIZE_MASK;
	}

	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		skb_frag_t *frag = &skb_shinfo(skb)->frags[i];

		if (!skb_frag_size(frag))
			continue;

		if (!edmav2_tx_map_buf(txdesc_ring, *idx, NULL, frag,
				       skb_frag_size(frag)))
			return -ENOMEM;

		*idx = (*idx + 1) & EDMAV2_TX_RING_SIZE_MASK;
	}

	return 0;
}

/* Undo the mapping of the descriptors from @start up to, not including, @end. */
static void edmav2_tx_unmap_range(struct edmav2_txdesc_ring *txdesc_ring,
				  u32 start, u32 end)
{
	struct device *dev = txdesc_ring->priv->ppe_dev->dev;

	for (; start != end; start = (start + 1) & EDMAV2_TX_RING_SIZE_MASK)
		edmav2_tx_unmap_buf(dev, &txdesc_ring->bufs[start]);
}

/* Map every buffer of the frame, starting at the descriptor @start. Returns
 * the number of descriptors that the frame uses, or 0 when a mapping failed.
 */
static u32 edmav2_tx_map_skb(struct edmav2_txdesc_ring *txdesc_ring,
			     struct sk_buff *skb, u32 start)
{
	struct sk_buff *iter_skb;
	u32 idx = start;

	if (edmav2_tx_map_one(txdesc_ring, skb, &idx))
		goto err;

	skb_walk_frags(skb, iter_skb) {
		if (edmav2_tx_map_one(txdesc_ring, iter_skb, &idx))
			goto err;
	}

	return (idx - start) & EDMAV2_TX_RING_SIZE_MASK;

err:
	edmav2_tx_unmap_range(txdesc_ring, start, idx);

	return 0;
}

/* Offloads and the destination of the frame, set on the first descriptor. */
static void edmav2_tx_fill_desc(struct edmav2_txdesc_pri *txd,
				struct sk_buff *skb, u32 dst_port)
{
	/* Offload L3/L4 checksum computation. */
	if (likely(skb->ip_summed == CHECKSUM_PARTIAL)) {
		EDMAV2_TXDESC_ADV_OFFLOAD_SET(txd);
		EDMAV2_TXDESC_IP_CSUM_SET(txd);
		EDMAV2_TXDESC_L4_CSUM_SET(txd);
	}

	/* Check if the packet needs TSO
	 * This will be mostly true for SG packets.
	 */
	if (unlikely(skb_is_gso(skb))) {
		if ((skb_shinfo(skb)->gso_type == SKB_GSO_TCPV4) ||
		    (skb_shinfo(skb)->gso_type == SKB_GSO_TCPV6)) {
			u32 mss = skb_shinfo(skb)->gso_size;

			/* If MSS<256, HW will do TSO using MSS=256,
			 * if MSS>10K, HW will do TSO using MSS=10K,
			 * else HW will report error 0x200000 in Tx Cmpl.
			 */
			mss = clamp(mss, EDMAV2_TX_TSO_MSS_MIN,
				    EDMAV2_TX_TSO_MSS_MAX);

			EDMAV2_TXDESC_TSO_ENABLE_SET(txd, 1);
			EDMAV2_TXDESC_MSS_SET(txd, mss);
		}
	}

	/* Set destination information in the descriptor. */
	EDMAV2_TXDESC_SERVICE_CODE_SET(txd, PPE_EDMA_SC_BYPASS_ID);
	EDMAV2_DST_INFO_SET(txd, dst_port);
}

static u32 edmav2_tx_avail_desc(struct edmav2_txdesc_ring *txdesc_ring,
				u32 hw_next_to_use)
{
	struct edmav2 *priv = txdesc_ring->priv;
	u32 idx_mask = priv->hw_info->idx_mask;
	u32 data = 0, hw_next_to_clean;
	u32 reg;

	reg = EDMA_BASE_OFFSET + EDMAV2_REG_TXDESC_CONS_IDX(txdesc_ring->id);
	regmap_read(priv->ppe_dev->regmap, reg, &data);

	hw_next_to_clean = data & idx_mask;

	return EDMAV2_DESC_AVAIL_COUNT(hw_next_to_clean - 1,
				       hw_next_to_use, EDMAV2_TX_RING_SIZE);
}

/* Transmit a frame through a Tx descriptor ring. The skb is consumed only
 * when EDMAV2_TX_OK is returned.
 */
static enum edmav2_tx_status edmav2_tx_ring_xmit(struct edmav2_txdesc_ring *txdesc_ring,
						 struct sk_buff *skb, u32 dst_port)
{
	struct edmav2_txdesc_stats *txdesc_stats = &txdesc_ring->txdesc_stats;
	struct edmav2 *priv = txdesc_ring->priv;
	const struct edmav2_hw_info *hw_info = priv->hw_info;
	struct ppe_device *ppe_dev = priv->ppe_dev;
	struct regmap *regmap = ppe_dev->regmap;
	u32 idx_mask = hw_info->idx_mask;
	u32 num_desc_needed, num_desc, i;
	u32 hw_next_to_use, idx;
	u32 reg;

	hw_next_to_use = txdesc_ring->prod_idx;

	/* An upper bound of the descriptors that the frame needs. */
	num_desc_needed = edmav2_tx_num_descs_for_sg(skb);

	/* HW does not support TSO for packets with more than 32 segments.
	 * HW hangs up if it sees more than 32 segments. Kernel Perform GSO
	 * for such packets with netdev gso_max_segs set to 32.
	 */
	if (unlikely(num_desc_needed > hw_info->tso_max)) {
		u64_stats_update_begin(&txdesc_stats->syncp);
		++txdesc_stats->tso_max_seg_exceed;
		u64_stats_update_end(&txdesc_stats->syncp);

		return EDMAV2_TX_FAIL;
	}

	if (unlikely(num_desc_needed > txdesc_ring->avail_desc)) {
		txdesc_ring->avail_desc = edmav2_tx_avail_desc(txdesc_ring,
							       hw_next_to_use);
		if (num_desc_needed > txdesc_ring->avail_desc) {
			u64_stats_update_begin(&txdesc_stats->syncp);
			++txdesc_stats->no_desc_avail;
			u64_stats_update_end(&txdesc_stats->syncp);

			return EDMAV2_TX_FAIL_NO_DESC;
		}
	}

	num_desc = edmav2_tx_map_skb(txdesc_ring, skb, hw_next_to_use);
	if (unlikely(!num_desc))
		return EDMAV2_TX_FAIL;

	idx = hw_next_to_use;
	for (i = 0; i < num_desc; i++) {
		struct edmav2_txdesc_pri *txd = EDMAV2_TXDESC_PRI_DESC(txdesc_ring, idx);

		memset(txd, 0, sizeof(*txd));
		EDMAV2_TXDESC_BUFFER_ADDR_SET(txd, txdesc_ring->bufs[idx].dma);
		EDMAV2_TXDESC_DATA_LEN_SET(txd, txdesc_ring->bufs[idx].len);

		if (i == 0)
			edmav2_tx_fill_desc(txd, skb, dst_port);

		if (i + 1 < num_desc) {
			EDMAV2_TXDESC_MORE_BIT_SET(txd, 1);
		} else {
			/* The last descriptor of the frame carries the skb. */
			EDMAV2_TXDESC_OPAQUE_SET(txd, skb);
		}

		EDMAV2_TXDESC_ENDIAN_SET(txd);
		idx = (idx + 1) & EDMAV2_TX_RING_SIZE_MASK;
	}

	txdesc_ring->prod_idx = idx & idx_mask;
	txdesc_ring->avail_desc -= num_desc;

	dsb(st);

	reg = EDMA_BASE_OFFSET + EDMAV2_REG_TXDESC_PROD_IDX(txdesc_ring->id);
	regmap_write(regmap, reg, txdesc_ring->prod_idx);

	return EDMAV2_TX_OK;
}

static void edmav2_tx_drop(struct net_device *netdev, struct sk_buff *skb)
{
	dev_kfree_skb_any(skb);
	DEV_STATS_INC(netdev, tx_dropped);
}

/**
 * edmav2_xmit - Transmit a frame.
 * @edma: EDMA
 * @skb: Socket buffer of the frame, its dev is the netdev of the port
 * @dst_port: Destination port
 * @txq: Transmit queue of the netdev
 *
 * Return NETDEV_TX_OK when the skb is consumed, NETDEV_TX_BUSY when it has to
 * be requeued after the queue was stopped.
 */
netdev_tx_t edmav2_xmit(struct edma *edma, struct sk_buff *skb, u8 dst_port,
			u8 txq)
{
	struct edmav2 *priv = edma->priv;
	struct net_device *netdev = skb->dev;
	struct edmav2_txdesc_ring *txdesc_ring;
	enum edmav2_tx_gso_status result;
	struct sk_buff *segs = NULL;
	u32 ncpu = num_possible_cpus();
	enum edmav2_tx_status ret;
	unsigned int len;

	if (unlikely(dst_port < EDMAV2_START_IFNUM ||
		     dst_port > priv->hw_info->max_ports)) {
		edmav2_tx_drop(netdev, skb);
		return NETDEV_TX_OK;
	}

	txq %= ncpu;
	txdesc_ring = &priv->tx_rings[(dst_port - EDMAV2_START_IFNUM) * ncpu + txq];

	result = edmav2_tx_gso_segment(priv, skb, netdev, &segs);
	if (likely(result == EDMAV2_TX_GSO_NOT_NEEDED)) {
		/* The skb can complete before the call returns. */
		len = skb->len;

		ret = edmav2_tx_ring_xmit(txdesc_ring, skb, dst_port);
		if (unlikely(ret == EDMAV2_TX_FAIL_NO_DESC)) {
			struct netdev_queue *nq = netdev_get_tx_queue(netdev, txq);

			netif_tx_stop_queue(nq);

			/* The completion may have freed descriptors before the
			 * queue was stopped, and nothing else wakes it.
			 */
			smp_mb();
			if (edmav2_tx_avail_desc(txdesc_ring, txdesc_ring->prod_idx))
				netif_tx_wake_queue(nq);

			return NETDEV_TX_BUSY;
		}

		if (unlikely(ret != EDMAV2_TX_OK))
			edmav2_tx_drop(netdev, skb);
		else
			dev_sw_netstats_tx_add(netdev, 1, len);

		return NETDEV_TX_OK;
	}

	if (unlikely(result == EDMAV2_TX_GSO_FAIL)) {
		edmav2_tx_drop(netdev, skb);
		return NETDEV_TX_OK;
	}

	dev_kfree_skb_any(skb);
	while (segs) {
		skb = segs;
		segs = segs->next;
		skb_mark_not_on_list(skb);

		len = skb->len;
		ret = edmav2_tx_ring_xmit(txdesc_ring, skb, dst_port);
		if (unlikely(ret != EDMAV2_TX_OK))
			edmav2_tx_drop(netdev, skb);
		else
			dev_sw_netstats_tx_add(netdev, 1, len);
	}

	return NETDEV_TX_OK;
}
