// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

/* TX datapath: descriptor fill, checksum/TSO, transmit, NAPI poll,
 * completion/release.
 */

#include <linux/if_vlan.h>
#include <linux/interrupt.h>
#include <linux/regmap.h>
#include <net/netdev_queues.h>

#include "edma.h"
#include "cfg_tx.h"

/* Each ring has its own handler and its own dev_id, and only the interrupt of
 * a ring schedules its NAPI.
 */
irqreturn_t edmav1_tx_irq_handle(int irq, void *ctx)
{
	struct edmav1_tx_queue *txq = ctx;
	u32 val;

	edma_read(txq->priv->edma,
		  EDMAV1_REG_TX_INT_STAT(txq->priv->soc->tx_int_base,
					 txq->cmpl_ring_index),
		    &val);
	if (!val)
		return IRQ_NONE;

	edmav1_tx_irq_mask_one(txq->priv, txq->cmpl_ring_index);

	if (likely(napi_schedule_prep(&txq->napi)))
		__napi_schedule(&txq->napi);

	return IRQ_HANDLED;
}

/* Descriptors the transmit ring has left, taken from the hardware rather than
 * from a cached index: the stop and its recheck have to see what the engine
 * has consumed by now, not what it had consumed when the frame arrived.
 */
u16 edmav1_txdesc_free(struct edmav1_tx_queue *txq)
{
	u32 prod, cons;

	edma_read(txq->priv->edma, EDMAV1_REG_TXDESC_PROD_IDX(txq->ring_index),
		  &prod);
	edma_read(txq->priv->edma, EDMAV1_REG_TXDESC_CONS_IDX(txq->ring_index),
		  &cons);

	return ((cons & EDMAV1_TXDESC_CONS_IDX_MASK) -
		(prod & EDMAV1_TXDESC_PROD_IDX_MASK) - 1) &
	       (txq->txdesc_ring.count - 1);
}

/* Unmaps every descriptor the frame was written from and releases the slots
 * that named them, returning the bytes the frame carried. A slot goes back
 * only once its descriptor has been read, so that a transmit taking the slot
 * cannot place a frame over what this walk has yet to reach.
 */
u32 edmav1_tx_release(struct edmav1_tx_queue *txq, u32 idx, struct sk_buff *skb,
		      int napi_budget)
{
	struct edmav1_ring *ring = &txq->txdesc_ring;
	struct device *dev = txq->priv->edma->dev;
	struct edmav1_txdesc *txdesc;
	u32 bytes, len;

	txdesc = EDMAV1_TXDESC_DESC(ring, idx);
	len = skb_headlen(skb);

	dma_unmap_single(dev, le32_to_cpu(txdesc->buffer_addr), len,
			 DMA_TO_DEVICE);
	bytes = len - EDMAV1_TX_PREHDR_SIZE;
	ring->skb_store[idx] = NULL;

	idx = (idx + 1) & (ring->count - 1);
	while (ring->skb_store[idx] == skb) {
		txdesc = EDMAV1_TXDESC_DESC(ring, idx);
		len = txdesc->word1 & EDMAV1_TXDESC_DATA_LENGTH_MASK;

		dma_unmap_page(dev, le32_to_cpu(txdesc->buffer_addr), len,
			       DMA_TO_DEVICE);
		bytes += len;
		ring->skb_store[idx] = NULL;
		idx = (idx + 1) & (ring->count - 1);
	}

	napi_consume_skb(skb, napi_budget);

	return bytes;
}

/* @napi_budget is the NAPI budget the poll was given, or zero when the caller
 * is not a poll: the skb cache napi_consume_skb() recycles into is per-CPU and
 * is only safe to touch from softirq context.
 */
u32 edmav1_clean_tx(struct edmav1_tx_queue *txq, int budget, int napi_budget)
{
	struct edmav1 *priv = txq->priv;
	const struct edmav1_soc_data *soc = priv->soc;
	struct edmav1_ring *txcmpl_ring = &txq->txcmpl_ring;
	u8 ring = txq->cmpl_ring_index;
	struct device *dev = priv->edma->dev;
	struct net_device *netdev = NULL;
	u32 cleaned = 0, pkts = 0, bytes = 0;
	struct edmav1_txcmpl *txcmpl;
	struct sk_buff *skb;
	u16 prod, cons;
	u32 val;

	edma_read(priv->edma, EDMAV1_REG_TXCMPL_PROD_IDX(soc->txcmpl_base, ring),
		  &val);
	prod = val & EDMAV1_TXCMPL_PROD_IDX_MASK;

	edma_read(priv->edma, EDMAV1_REG_TXCMPL_CONS_IDX(soc->txcmpl_base, ring),
		  &val);
	cons = val & EDMAV1_TXCMPL_CONS_IDX_MASK;

	while (cons != prod && cleaned < budget) {
		txcmpl = EDMAV1_TXCMPL_DESC(txcmpl_ring, cons);

		if (unlikely(txcmpl->status & EDMAV1_TXCMPL_ERROR)) {
			dev_warn_ratelimited(dev, "tx error %#x\n",
					     txcmpl->status);
			priv->stats.tx_desc_error++;
			EDMAV1_DEV_STATS_INC(priv, tx_errors);
		}

		/* A frame is named by the first completion of its run and
		 * released on the one that clears the more bit; the opaque of
		 * the completions in between is not written.
		 */
		if (!txq->txcmpl_run) {
			txq->txcmpl_run = true;
			txq->txcmpl_idx = txcmpl->buffer_addr;
			if (txq->txcmpl_idx < txq->txdesc_ring.count)
				txq->txcmpl_skb =
					txq->txdesc_ring.skb_store[txq->txcmpl_idx];
		}

		if (!(txcmpl->status & EDMAV1_TXCMPL_MORE)) {
			skb = txq->txcmpl_skb;
			txq->txcmpl_skb = NULL;
			txq->txcmpl_run = false;

			if (unlikely(!skb)) {
				dev_warn(dev,
					 "invalid skb: cons:%u prod:%u status %x\n",
					 cons, prod, txcmpl->status);
				priv->stats.tx_unnamed_frame++;
			} else {
				netdev = skb->dev;
				bytes += edmav1_tx_release(txq, txq->txcmpl_idx, skb,
							 napi_budget);
				pkts++;
			}
		}

		if (++cons == txcmpl_ring->count)
			cons = 0;

		cleaned++;
	}

	if (cleaned == 0)
		return 0;

	/* A drain runs with the queue deliberately stopped and the rings about
	 * to be freed, so only a poll may wake it. The netdev is the one that
	 * transmitted the frames.
	 */
	if (netdev)
		__netif_txq_completed_wake(netdev_get_tx_queue(netdev,
							       txq - priv->txq),
					   pkts, bytes, edmav1_txdesc_free(txq),
					   EDMAV1_TX_RING_THRESH, !napi_budget);

	/* Ensure all TX completions are processed before updating cons idx */
	wmb();
	edma_write(priv->edma, EDMAV1_REG_TXCMPL_CONS_IDX(soc->txcmpl_base, ring),
		   cons);

	return cleaned;
}

int edmav1_tx_napi(struct napi_struct *napi, int budget)
{
	struct edmav1_tx_queue *txq = container_of(napi, struct edmav1_tx_queue, napi);
	int work = edmav1_clean_tx(txq, budget, budget);
	u32 val;

	if (work < budget) {
		edma_read(txq->priv->edma,
			  EDMAV1_REG_TX_INT_STAT(txq->priv->soc->tx_int_base,
						 txq->cmpl_ring_index),
			  &val);
		if (val)
			return budget;

		if (napi_complete_done(napi, work))
			edmav1_tx_irq_unmask_one(txq->priv, txq->cmpl_ring_index);
	}

	return work;
}

/* The engine generates the transport checksum of an outgoing TCP or UDP frame,
 * and the IPv4 header checksum with it. It parses the frame for the offsets
 * rather than being handed a start and an offset, so the offload is announced
 * per protocol.
 */
static void edmav1_tx_csum(struct sk_buff *skb, struct edmav1_tx_preheader *txph,
			   __be16 proto)
{
	if (skb->ip_summed != CHECKSUM_PARTIAL)
		return;

	txph->tx_pre4 |= EDMAV1_TX_PRE4_ADV_OFFLOAD_EN;
	txph->tx_pre6 |= EDMAV1_TX_PRE6_CSUM_MODE_L4;

	if (proto == htons(ETH_P_IP))
		txph->tx_pre6 |= EDMAV1_TX_PRE6_IP_CSUM_EN;
}

/* Segmentation is one bit in every descriptor of the frame and the segment
 * size in the preheader. The engine writes the headers of each segment it
 * cuts, so the checksums it is already asked for cover what it produced.
 */
static u32 edmav1_tx_tso(struct sk_buff *skb, struct edmav1_tx_preheader *txph)
{
	if (!skb_is_gso(skb))
		return 0;

	txph->tx_pre6 |= skb_shinfo(skb)->gso_size & EDMAV1_TX_PRE6_MSS_MASK;

	return EDMAV1_TXDESC_TSO_EN;
}

static netdev_tx_t edmav1_ring_xmit(struct edmav1_tx_queue *txq,
				    struct sk_buff *skb, u8 dst_port)
{
	struct net_device *netdev = skb->dev;
	struct edmav1_ring *txdesc_ring = &txq->txdesc_ring;
	const struct skb_shared_info *shinfo = skb_shinfo(skb);
	struct edmav1 *priv = txq->priv;
	struct device *dev = priv->edma->dev;
	u16 mask = txdesc_ring->count - 1;
	unsigned int qid = txq - priv->txq;
	struct edmav1_tx_preheader *txph;
	u16 ndesc = shinfo->nr_frags + 1;
	struct edmav1_txdesc *txdesc;
	u16 prod, cons;
	u32 val, idx, i, len, bytes, tso;
	dma_addr_t head_dma;
	bool taken = false;
	__be16 proto;

	spin_lock_bh(&txq->tx_lock);

	edma_read(priv->edma, EDMAV1_REG_TXDESC_PROD_IDX(txq->ring_index),
		  &val);
	prod = val & EDMAV1_TXDESC_PROD_IDX_MASK;

	edma_read(priv->edma, EDMAV1_REG_TXDESC_CONS_IDX(txq->ring_index),
		  &val);
	cons = val & EDMAV1_TXDESC_CONS_IDX_MASK;

	idx = prod & mask;

	/* A frame holds every store slot it spans, not only the one its
	 * preheader names, so that a later frame placed over its descriptors
	 * cannot change the addresses its completion still has to unmap.
	 */
	for (i = 0; i < ndesc; i++)
		taken |= !!txdesc_ring->skb_store[(idx + i) & mask];

	/* Both refusals come before the preheader is pushed: the qdisc requeues
	 * the frame as it was handed over, and a second push would prefix it
	 * twice and hand the hardware a length that no longer describes it.
	 */
	if (taken || ((cons - prod - 1) & mask) < ndesc)
		goto busy;

	/* The hardware wants the port in the preheader and not in the frame.
	 * The tag goes before vlan_get_protocol() reads the MAC header.
	 */
	if (priv->edma->tag_mode == EDMA_TAG_DSA)
		edma_tx_tag_strip(skb);

	len = skb_headlen(skb);
	bytes = skb->len;

	proto = vlan_get_protocol(skb);

	txph = (struct edmav1_tx_preheader *)skb_push(skb, EDMAV1_TX_PREHDR_SIZE);
	memset((void *)txph, 0, EDMAV1_TX_PREHDR_SIZE);

	txph->dst_info = (EDMAV1_DST_PORT_TYPE << 8) |
			 (dst_port & EDMAV1_DST_PORT_ID_MASK);
	edmav1_tx_csum(skb, txph, proto);
	tso = edmav1_tx_tso(skb, txph);

	txdesc_ring->skb_store[idx] = skb;
	txph->opaque = idx;

	head_dma = dma_map_single(dev, skb->data, len + EDMAV1_TX_PREHDR_SIZE,
				  DMA_TO_DEVICE);
	if (dma_mapping_error(dev, head_dma)) {
		txdesc_ring->skb_store[idx] = NULL;
		goto drop;
	}

	txdesc = EDMAV1_TXDESC_DESC(txdesc_ring, idx);
	txdesc->buffer_addr = cpu_to_le32(head_dma);
	txdesc->word1 = tso | (1 << EDMAV1_TXDESC_PREHEADER_SHIFT) |
			(ndesc > 1 ? EDMAV1_TXDESC_MORE : 0) |
			((EDMAV1_TX_PREHDR_SIZE & EDMAV1_TXDESC_DATA_OFFSET_MASK)
			 << EDMAV1_TXDESC_DATA_OFFSET_SHIFT) |
			(len & EDMAV1_TXDESC_DATA_LENGTH_MASK);

	for (i = 0; i < shinfo->nr_frags; i++) {
		const skb_frag_t *frag = &shinfo->frags[i];
		u32 fidx = (idx + 1 + i) & mask;
		dma_addr_t dma;

		len = skb_frag_size(frag);
		dma = skb_frag_dma_map(dev, frag, 0, len, DMA_TO_DEVICE);
		if (dma_mapping_error(dev, dma))
			goto unmap;

		txdesc_ring->skb_store[fidx] = skb;
		txdesc = EDMAV1_TXDESC_DESC(txdesc_ring, fidx);
		txdesc->buffer_addr = cpu_to_le32(dma);
		txdesc->word1 = tso | (i + 1 < shinfo->nr_frags ?
				       EDMAV1_TXDESC_MORE : 0) |
				(len & EDMAV1_TXDESC_DATA_LENGTH_MASK);
	}

	prod = (prod + ndesc) & mask;

	dev_sw_netstats_tx_add(netdev, 1, bytes);
	netdev_tx_sent_queue(netdev_get_tx_queue(netdev, qid), bytes);

	/* Ensure descriptor writes are visible before updating prod idx */
	wmb();
	edma_write(priv->edma, EDMAV1_REG_TXDESC_PROD_IDX(txq->ring_index),
		   prod & EDMAV1_TXDESC_PROD_IDX_MASK);

	/* The queue is rechecked against the hardware once it is stopped: a
	 * completion that drains the ring between the descriptor going out and
	 * the stop landing would otherwise find the queue still running and
	 * leave nothing behind to start it again. The indices this frame was
	 * placed from decide whether to stop at all, since a consumer index
	 * only ages into reporting less room than the ring has.
	 */
	if (((cons - prod - 1) & mask) < EDMAV1_TX_RING_THRESH)
		netif_txq_try_stop(netdev_get_tx_queue(netdev, qid),
				   edmav1_txdesc_free(txq),
				   EDMAV1_TX_RING_THRESH);

	spin_unlock_bh(&txq->tx_lock);
	return NETDEV_TX_OK;

unmap:
	while (i--) {
		u32 fidx = (idx + 1 + i) & mask;

		txdesc = EDMAV1_TXDESC_DESC(txdesc_ring, fidx);
		dma_unmap_page(dev, le32_to_cpu(txdesc->buffer_addr),
			       txdesc->word1 & EDMAV1_TXDESC_DATA_LENGTH_MASK,
			       DMA_TO_DEVICE);
		txdesc_ring->skb_store[fidx] = NULL;
	}

	dma_unmap_single(dev, head_dma, skb_headlen(skb), DMA_TO_DEVICE);
	txdesc_ring->skb_store[idx] = NULL;
drop:
	dev_kfree_skb_any(skb);
	spin_unlock_bh(&txq->tx_lock);
	return NETDEV_TX_OK;

busy:
	/* A refusal stops the queue here rather than in the caller, so that the
	 * recheck happens against the state this refusal was decided on: a
	 * completion that drains the ring once the lock is dropped would
	 * otherwise find the queue still running and leave nothing behind to
	 * start it again. A taken store slot outlives the descriptor that named
	 * it, because the engine releases the descriptor as soon as it reads it
	 * and only the completion clears the slot, so a free descriptor count
	 * would restart the queue on a resource the refused frame still lacks.
	 */
	netif_txq_try_stop(netdev_get_tx_queue(netdev, qid),
			   taken ? 0 : edmav1_txdesc_free(txq),
			   EDMAV1_TX_RING_THRESH);

	spin_unlock_bh(&txq->tx_lock);
	return NETDEV_TX_BUSY;
}

/* A frame outside the bounds it may be described within is made linear
 * instead. The last buffer has no length floor, and the head grows by the
 * preheader before it is mapped, so neither is counted here.
 */
static bool edmav1_tx_needs_linearize(const struct sk_buff *skb)
{
	const struct skb_shared_info *shinfo = skb_shinfo(skb);
	int i;

	if (shinfo->nr_frags + 1 > EDMAV1_TX_MAX_SEGS)
		return true;
	for (i = 0; i + 1 < shinfo->nr_frags; i++)
		if (skb_frag_size(&shinfo->frags[i]) < EDMAV1_TX_MIN_SEG)
			return true;
	return false;
}

netdev_tx_t edmav1_xmit(struct edma *edma, struct sk_buff *skb, u8 dst_port,
			u8 txq)
{
	struct edmav1 *priv = edma->priv;
	const struct edmav1_soc_data *soc = priv->soc;
	struct net_device *netdev = skb->dev;

	if (skb->len < ETH_HLEN)
		goto drop;

	if (edmav1_tx_needs_linearize(skb) && skb_linearize(skb))
		goto drop;

	/* skb_padto() zero-fills the tailroom but leaves the tail where it
	 * was, so advancing the length by hand puts skb->len past the data a
	 * later head reallocation copies: the pad would be reallocated
	 * uninitialised and transmitted.
	 */
	if (soc->tx_min_size && skb_put_padto(skb, soc->tx_min_size)) {
		DEV_STATS_INC(netdev, tx_dropped);
		return NETDEV_TX_OK;
	}

	if ((skb_cloned(skb) || skb_headroom(skb) < EDMAV1_TX_PREHDR_SIZE) &&
	    pskb_expand_head(skb, EDMAV1_TX_PREHDR_SIZE, 0, GFP_ATOMIC))
		goto drop;

	if (txq >= priv->num_tx_queues)
		txq = 0;

	return edmav1_ring_xmit(&priv->txq[txq], skb, dst_port);

drop:
	dev_kfree_skb_any(skb);
	DEV_STATS_INC(netdev, tx_dropped);

	return NETDEV_TX_OK;
}
