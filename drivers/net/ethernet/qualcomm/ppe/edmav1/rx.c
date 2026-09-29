// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

/* RX datapath: buffer fill, page tracking, RSS hash tagging, NAPI poll. */

#include <linux/hash.h>
#include <linux/if_vlan.h>
#include <linux/interrupt.h>
#include <linux/regmap.h>

#include "edma.h"
#include "cfg_rx.h"

/* Each ring has its own handler and its own dev_id, and only the interrupt of
 * a ring schedules its NAPI. A handler that scans every ring would schedule
 * the NAPI of a ring on the CPU that took the interrupt, and not on the CPU
 * that serves the ring.
 */
irqreturn_t edmav1_rxdesc_irq_handle(int irq, void *ctx)
{
	struct edmav1_rx_queue *rxq = ctx;
	u32 val;

	edma_read(rxq->priv->edma, EDMAV1_REG_RXDESC_INT_STAT(rxq->ring_index),
		  &val);
	if (!val)
		return IRQ_NONE;

	edmav1_rxdesc_irq_mask_one(rxq->priv, rxq->ring_index);

	if (likely(napi_schedule_prep(&rxq->napi)))
		__napi_schedule(&rxq->napi);

	return IRQ_HANDLED;
}

/* The one shared RXFILL line (rxq[0] only - see edma.h) piggybacks
 * on rxq[0]'s own NAPI, which already checks/replenishes that ring.
 */
irqreturn_t edmav1_rxfill_irq_handle(int irq, void *ctx)
{
	struct edmav1 *priv = ctx;
	const struct edmav1_soc_data *soc = priv->soc;
	u32 val;

	edma_read(priv->edma, EDMAV1_REG_RXFILL_INT_STAT(soc->rxfill_ring),
		  &val);
	if (!val)
		return IRQ_NONE;

	edma_write(priv->edma, EDMAV1_REG_RXFILL_INT_MASK(soc->rxfill_ring), 0);

	if (likely(napi_schedule_prep(&priv->rxq[0].napi)))
		__napi_schedule(&priv->rxq[0].napi);

	return IRQ_HANDLED;
}

int edmav1_rx_fill(struct edmav1_rx_queue *rxq)
{
	struct edmav1 *priv = rxq->priv;
	struct edmav1_ring *rxfill_ring = &rxq->rxfill_ring;
	u8 ring = rxq->rxfill_ring_index;
	struct edmav1_rxfill_desc *rxfill_desc;
	struct edmav1_rx_preheader *rxph;
	u16 prod, cons, next;
	struct page *page;
	u16 filled = 0;
	dma_addr_t dma;
	u32 val;

	edma_read(priv->edma, EDMAV1_REG_RXFILL_PROD_IDX(ring), &val);
	prod = val & EDMAV1_RXFILL_PROD_IDX_MASK & (rxfill_ring->count - 1);

	edma_read(priv->edma, EDMAV1_REG_RXFILL_CONS_IDX(ring), &val);
	cons = val & EDMAV1_RXFILL_CONS_IDX_MASK & (rxfill_ring->count - 1);

	while (1) {
		next = prod + 1;
		if (next == rxfill_ring->count)
			next = 0;

		if (next == cons)
			break;
		/* The page may still be prefetched inside EDMA. */
		if (unlikely(rxfill_ring->page_store[prod]))
			break;

		page = page_pool_dev_alloc_pages(rxq->page_pool);
		if (unlikely(!page))
			break;

		rxfill_desc = EDMAV1_RXFILL_DESC(rxfill_ring, prod);

		dma = page_pool_get_dma_addr(page) + NET_SKB_PAD;
		rxph = page_address(page) + NET_SKB_PAD;
		rxph->opaque = cpu_to_le32(prod);
		dma_sync_single_for_device(priv->edma->dev, dma,
					   sizeof(rxph->opaque), DMA_FROM_DEVICE);
		rxfill_ring->page_store[prod] = page;
		rxfill_desc->buffer_addr = cpu_to_le32(dma);
		rxfill_desc->word1 = cpu_to_le32(priv->rx_buffer_size &
						 EDMAV1_RXFILL_BUF_SIZE_MASK);

		filled++;
		prod = next;
	}

	if (filled) {
		/* The descriptors are visible before the producer index. */
		wmb();
		edma_write(priv->edma, EDMAV1_REG_RXFILL_PROD_IDX(ring),
			   prod & EDMAV1_RXFILL_PROD_IDX_MASK);
	}

	return filled;
}

bool edmav1_rx_page_take(struct edmav1_rx_queue *rxq, struct page *page,
			 u32 store_idx)
{
	struct edmav1_ring *ring = &rxq->rxfill_ring;
	int i;

	if (likely(store_idx < ring->count &&
		   ring->page_store[store_idx] == page)) {
		ring->page_store[store_idx] = NULL;
		return true;
	}

	for (i = 0; i < ring->count; i++) {
		if (ring->page_store[i] != page)
			continue;

		ring->page_store[i] = NULL;
		dev_warn_ratelimited(rxq->priv->edma->dev,
				     "rx page has invalid store index %u, expected %d\n",
				     store_idx, i);
		return true;
	}

	/* The page may already belong to an skb, so it cannot be freed safely. */
	dev_warn_ratelimited(rxq->priv->edma->dev,
			     "rx page with store index %u is not tracked\n",
			     store_idx);
	return false;
}

/* The parser hashes the tuple it matched and says which tuple that was. The
 * same field takes other values that are not a tuple hash, so only the two
 * tuple verdicts are taken.
 *
 * The value arrives in the low bits of the word while the stack scales a hash
 * across the whole of it, so a hash left where the parser put it selects the
 * same receive queue for every flow. Spreading it over the word is one to one,
 * so the frames of a flow still land together.
 */
static void edmav1_rx_hash(struct sk_buff *skb,
			   const struct edmav1_rx_preheader *rxph)
{
	u32 pre2 = le32_to_cpu(rxph->rx_pre2);
	u8 flag;

	flag = (pre2 >> EDMAV1_RXPH_HASH_FLAG_SHIFT) & EDMAV1_RXPH_HASH_FLAG_MASK;
	if (flag != EDMAV1_RXPH_HASH_5TUPLE && flag != EDMAV1_RXPH_HASH_3TUPLE)
		return;

	skb_set_hash(skb, hash_32(pre2 & EDMAV1_RXPH_HASH_MASK, 32),
		     flag == EDMAV1_RXPH_HASH_5TUPLE ? PKT_HASH_TYPE_L4 :
						     PKT_HASH_TYPE_L3);
}

static u32 edmav1_clean_rx(struct edmav1_rx_queue *rxq, int budget)
{
	struct edmav1 *priv = rxq->priv;
	struct edmav1_ring *rxdesc_ring = &rxq->rxdesc_ring;
	u8 ring = rxq->ring_index;
	struct edmav1_rx_preheader *rxph;
	struct edmav1_rxdesc *rxdesc;
	struct sk_buff *skb;
	u16 prod, cons;
	struct page *page;
	u32 done = 0;
	u32 src_port;
	int pkt_len;
	u32 val;

	edma_read(priv->edma, EDMAV1_REG_RXDESC_PROD_IDX(ring), &val);
	prod = val & EDMAV1_RXDESC_PROD_IDX_MASK;

	edma_read(priv->edma, EDMAV1_REG_RXDESC_CONS_IDX(ring), &val);
	cons = val & EDMAV1_RXDESC_CONS_IDX_MASK;

	while (cons != prod && done < budget) {
		u32 desc_addr, desc_status;
		u32 store_idx;

		rxdesc = EDMAV1_RXDESC_DESC(rxdesc_ring, cons);
		desc_addr = le32_to_cpu(rxdesc->buffer_addr);
		desc_status = le32_to_cpu(rxdesc->status);
		rxph = phys_to_virt(desc_addr);
		page = virt_to_head_page(rxph);

		pkt_len = desc_status & EDMAV1_RXDESC_PACKET_LEN_MASK;

		if (unlikely(desc_status & EDMAV1_RXDESC_MORE))
			priv->stats.rx_split_frame++;

		page_pool_dma_sync_for_cpu(rxq->page_pool, page, 0,
					   EDMAV1_RX_PREHDR_SIZE + pkt_len);
		store_idx = le32_to_cpu(rxph->opaque);
		if (unlikely(!edmav1_rx_page_take(rxq, page, store_idx))) {
			priv->stats.rx_untracked_page++;
			EDMAV1_DEV_STATS_INC(priv, rx_errors);
			goto next;
		}

		src_port = rxph->src_info & EDMAV1_SRC_PORT_MASK;

		if (EDMAV1_RXPH_SRC_INFO_TYPE_GET(rxph) !=
		    EDMAV1_PREHDR_DSTINFO_PORTID_IND ||
		    src_port >= EDMA_MAX_PORTS) {
			dev_warn_ratelimited(priv->edma->dev,
					     "rx drop: src_info_type=%#x src_info=%#06x dst_info=%#06x\n",
					     EDMAV1_RXPH_SRC_INFO_TYPE_GET(rxph),
					     le16_to_cpu(rxph->src_info),
					     le16_to_cpu(rxph->dst_info));
			page_pool_put_full_page(rxq->page_pool, page, true);
			priv->stats.rx_bad_src_info++;
			EDMAV1_DEV_STATS_INC(priv, rx_errors);
			goto next;
		}

		skb = napi_build_skb(page_address(page), page_size(page));
		if (unlikely(!skb)) {
			page_pool_put_full_page(rxq->page_pool, page, true);
			priv->stats.rx_no_skb++;
			EDMAV1_DEV_STATS_INC(priv, rx_dropped);
			goto next;
		}

		skb_mark_for_recycle(skb);
		skb_reserve(skb, NET_SKB_PAD + EDMAV1_RX_PREHDR_SIZE);
		skb_put(skb, pkt_len);

		edmav1_rx_hash(skb, rxph);
		edma_rx_deliver(priv->edma, &rxq->napi, skb, src_port);

next:
		if (++cons == rxdesc_ring->count)
			cons = 0;

		done++;
	}

	edmav1_rx_fill(rxq);

	/* The descriptors are read before the consumer index frees them. */
	wmb();
	edma_write(priv->edma, EDMAV1_REG_RXDESC_CONS_IDX(ring), cons);
	return done;
}

/* Only rxq[0]'s rxfill ring has a real hardware IRQ - it alone needs its
 * RXFILL_INT_STAT/MASK checked/re-armed here. But every queue's rxfill ring
 * can still run dry under memory pressure, and with no traffic to trigger a
 * fresh rxdesc IRQ that would otherwise stall the queue forever, so every
 * queue self-checks its own ring for starvation before letting its NAPI go
 * idle.
 */
int edmav1_rx_napi(struct napi_struct *napi, int budget)
{
	struct edmav1_rx_queue *rxq = container_of(napi, struct edmav1_rx_queue, napi);
	struct edmav1 *priv = rxq->priv;
	const struct edmav1_soc_data *soc = priv->soc;
	u8 ring = rxq->ring_index;
	int done;

	done = edmav1_clean_rx(rxq, budget);

	if (done < budget) {
		u8 fill_ring = rxq->rxfill_ring_index;
		u16 prod, cons;
		u32 val;

		edma_read(priv->edma, EDMAV1_REG_RXDESC_INT_STAT(ring), &val);
		if (val)
			return budget;

		if (rxq == &priv->rxq[0]) {
			edma_read(priv->edma,
				  EDMAV1_REG_RXFILL_INT_STAT(soc->rxfill_ring),
				    &val);
			if (val)
				return budget;
		}

		edma_read(priv->edma, EDMAV1_REG_RXFILL_PROD_IDX(fill_ring),
			  &val);
		prod = val & (rxq->rxfill_ring.count - 1);
		edma_read(priv->edma, EDMAV1_REG_RXFILL_CONS_IDX(fill_ring),
			  &val);
		cons = val & (rxq->rxfill_ring.count - 1);
		if (prod == cons) {
			dev_warn_ratelimited(priv->edma->dev,
					     "rx queue %u RXFILL ring starved\n",
					     (unsigned int)(rxq - priv->rxq));
			priv->stats.rx_fill_starved++;
			EDMAV1_DEV_STATS_INC(priv, rx_missed_errors);
			edmav1_rx_fill(rxq);
			return budget;
		}

		if (napi_complete_done(napi, done)) {
			edmav1_rxdesc_irq_unmask_one(priv, ring);
			if (rxq == &priv->rxq[0])
				edma_write(priv->edma,
					   EDMAV1_REG_RXFILL_INT_MASK(soc->rxfill_ring),
					     EDMAV1_RXFILL_INT_MASK);
		}
	}

	return done;
}
