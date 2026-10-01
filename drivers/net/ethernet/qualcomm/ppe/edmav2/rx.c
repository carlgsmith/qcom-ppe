// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

/* Provides APIs to alloc Rx Buffers, reap the buffers, receive and
 * process linear and Scatter Gather packets.
 */

#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/irqreturn.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/regmap.h>

#include "../ppe.h"
#include "cfg_rx.h"
#include "edma.h"
#include "regs.h"

static int edmav2_rx_alloc_buffer_list(struct edmav2_rxfill_ring *rxfill_ring, int alloc_count)
{
	struct edmav2 *priv = rxfill_ring->priv;
	struct edmav2_rxfill_stats *rxfill_stats = &rxfill_ring->rxfill_stats;
	struct ppe_device *ppe_dev = priv->ppe_dev;
	u32 rx_alloc_size = rxfill_ring->alloc_size;
	struct regmap *regmap = ppe_dev->regmap;
	struct edmav2_rxfill_desc *rxfill_desc;
	u32 buf_len = rxfill_ring->buf_len;
	struct device *dev = ppe_dev->dev;
	u16 prod_idx;
	u16 num_alloc = 0;
	u32 dma_map_size;
	u32 reg;

	prod_idx = rxfill_ring->prod_idx;
	dma_map_size = rx_alloc_size - EDMAV2_RX_SKB_HEADROOM - NET_IP_ALIGN;

	while (likely(alloc_count--)) {
		dma_addr_t buff_addr;
		struct sk_buff *skb;

		rxfill_desc = EDMAV2_RXFILL_DESC(rxfill_ring, prod_idx);

		skb = dev_alloc_skb(rx_alloc_size);
		if (unlikely(!skb)) {
			u64_stats_update_begin(&rxfill_stats->syncp);
			++rxfill_stats->alloc_failed;
			u64_stats_update_end(&rxfill_stats->syncp);
			break;
		}

		skb_reserve(skb, EDMAV2_RX_SKB_HEADROOM + NET_IP_ALIGN);

		buff_addr = dma_map_single(dev, skb->data, dma_map_size, DMA_FROM_DEVICE);
		if (dma_mapping_error(dev, buff_addr)) {
			dev_dbg(dev, "Unable to map an Rx buffer\n");
			dev_kfree_skb_any(skb);
			break;
		}

		EDMAV2_RXFILL_BUFFER_ADDR_SET(rxfill_desc, buff_addr);

		EDMAV2_RXFILL_OPAQUE_LO_SET(rxfill_desc, skb);
#ifdef __LP64__
		EDMAV2_RXFILL_OPAQUE_HI_SET(rxfill_desc, skb);
#endif
		EDMAV2_RXFILL_PACKET_LEN_SET(rxfill_desc,
					     (u32)(buf_len) & EDMAV2_RXFILL_BUF_SIZE_MASK);
		prod_idx = (prod_idx + 1) & EDMAV2_RX_RING_SIZE_MASK;
		num_alloc++;

		EDMAV2_RXFILL_ENDIAN_SET(rxfill_desc);
	}

	if (likely(num_alloc)) {
		dsb(st);
		reg = EDMA_BASE_OFFSET + EDMAV2_REG_RXFILL_PROD_IDX(rxfill_ring->ring_id);
		regmap_write(regmap, reg, prod_idx);
		rxfill_ring->prod_idx = prod_idx;
	}

	return num_alloc;
}

/**
 * edmav2_rx_alloc_buffer - EDMA Rx alloc buffer.
 * @rxfill_ring: EDMA Rxfill ring
 * @alloc_count: Number of rings to alloc
 *
 * Alloc Rx buffers for RxFill ring.
 *
 * Return the number of rings allocated.
 */
int edmav2_rx_alloc_buffer(struct edmav2_rxfill_ring *rxfill_ring, int alloc_count)
{
	return edmav2_rx_alloc_buffer_list(rxfill_ring, alloc_count);
}

static u8 edmav2_rx_checksum_verify(struct edmav2_rxdesc_pri *rxdesc_pri,
				    struct sk_buff *skb)
{
	u8 pid = EDMAV2_RXDESC_PID_GET(rxdesc_pri);

	skb_checksum_none_assert(skb);

	/* Mark ip_summed appropriately in the skb as per the L3/L4 checksum
	 * status in descriptor.
	 */
	if (likely(EDMAV2_RX_PID_IS_IPV4(pid))) {
		if (likely(EDMAV2_RXDESC_L3CSUM_STATUS_GET(rxdesc_pri)) &&
		    likely(EDMAV2_RXDESC_L4CSUM_STATUS_GET(rxdesc_pri)))
			return CHECKSUM_UNNECESSARY;
	} else if (likely(EDMAV2_RX_PID_IS_IPV6(pid))) {
		if (likely(EDMAV2_RXDESC_L4CSUM_STATUS_GET(rxdesc_pri)))
			return CHECKSUM_UNNECESSARY;
	}

	return skb->ip_summed;
}

/* The source port is in the source information of the first descriptor of
 * a frame. Returns the port number, or a negative value when the frame has to
 * be dropped.
 */
static int edmav2_rx_src_port(struct edmav2_rxdesc_ring *rxdesc_ring,
			      struct edmav2_rxdesc_pri *rxdesc_pri)
{
	struct edmav2 *priv = rxdesc_ring->priv;
	struct edmav2_rxdesc_stats *rxdesc_stats = &rxdesc_ring->rxdesc_stats;
	u32 src_info = EDMAV2_RXDESC_SRC_INFO_GET(rxdesc_pri);
	u32 src_port_num;

	if (unlikely((src_info & EDMAV2_RXDESC_SRCINFO_TYPE_MASK) !=
		     EDMAV2_RXDESC_SRCINFO_TYPE_PORTID)) {
		dev_warn_ratelimited(priv->ppe_dev->dev,
				     "Invalid src info_type: 0x%x\n",
				     src_info & EDMAV2_RXDESC_SRCINFO_TYPE_MASK);

		u64_stats_update_begin(&rxdesc_stats->syncp);
		++rxdesc_stats->src_port_inval_type;
		u64_stats_update_end(&rxdesc_stats->syncp);

		return -EINVAL;
	}

	src_port_num = src_info & EDMAV2_RXDESC_PORTNUM_BITS;
	if (unlikely(src_port_num < EDMAV2_START_IFNUM ||
		     src_port_num > priv->hw_info->max_ports)) {
		dev_warn_ratelimited(priv->ppe_dev->dev,
				     "Port number error: %u\n", src_port_num);

		u64_stats_update_begin(&rxdesc_stats->syncp);
		++rxdesc_stats->src_port_inval;
		u64_stats_update_end(&rxdesc_stats->syncp);

		return -EINVAL;
	}

	return src_port_num;
}

static void edmav2_rx_deliver(struct edmav2_rxdesc_ring *rxdesc_ring,
			      struct edmav2_rxdesc_pri *rxdesc_pri,
			      struct sk_buff *skb, u8 src_port)
{
	struct edmav2 *priv = rxdesc_ring->priv;
	struct net_device *netdev = edma_rx_netdev(priv->edma, src_port);

	if (unlikely(!netdev)) {
		u64_stats_update_begin(&rxdesc_ring->rxdesc_stats.syncp);
		++rxdesc_ring->rxdesc_stats.src_port_inval_netdev;
		u64_stats_update_end(&rxdesc_ring->rxdesc_stats.syncp);
		dev_kfree_skb_any(skb);
		return;
	}

	if (likely(netdev->features & NETIF_F_RXCSUM))
		skb->ip_summed = edmav2_rx_checksum_verify(rxdesc_pri, skb);

	edma_rx_deliver(priv->edma, &rxdesc_ring->napi, skb, src_port);
}

static void edmav2_rx_process_last_segment(struct edmav2_rxdesc_ring *rxdesc_ring,
					   struct edmav2_rxdesc_pri *rxdesc_pri)
{
	struct sk_buff *skb_head = rxdesc_ring->head;
	int src_port = rxdesc_ring->src_port;

	rxdesc_ring->head = NULL;
	rxdesc_ring->last = NULL;

	if (unlikely(!pskb_pull(skb_head, rxdesc_ring->data_offset))) {
		dev_kfree_skb_any(skb_head);
		return;
	}

	edmav2_rx_deliver(rxdesc_ring, rxdesc_pri, skb_head, src_port);
}

/* A frame that spans several descriptors is chained into the frag list of
 * the first skb. The source information and the data offset are only valid
 * in the first descriptor.
 */
static void edmav2_rx_handle_frag_list(struct edmav2_rxdesc_ring *rxdesc_ring,
				       struct edmav2_rxdesc_pri *rxdesc_pri,
				       struct sk_buff *skb, int src_port)
{
	u32 pkt_length = EDMAV2_RXDESC_PACKET_LEN_GET(rxdesc_pri);

	skb_put(skb, pkt_length);

	if (!rxdesc_ring->head) {
		rxdesc_ring->head = skb;
		rxdesc_ring->last = NULL;
		rxdesc_ring->src_port = src_port;
		rxdesc_ring->data_offset = EDMAV2_RXDESC_DATA_OFFSET_GET(rxdesc_pri);
		return;
	}

	/* Append it to the fraglist of head if this is second frame
	 * If not second frame append to tail.
	 */
	if (!skb_has_frag_list(rxdesc_ring->head))
		skb_shinfo(rxdesc_ring->head)->frag_list = skb;
	else
		rxdesc_ring->last->next = skb;

	rxdesc_ring->last = skb;
	rxdesc_ring->last->next = NULL;
	rxdesc_ring->head->len += pkt_length;
	rxdesc_ring->head->data_len += pkt_length;
	rxdesc_ring->head->truesize += skb->truesize;

	/* If there are more segments for this packet,
	 * then we have nothing to do. Otherwise process
	 * last segment and send packet to stack.
	 */
	if (EDMAV2_RXDESC_MORE_BIT_GET(rxdesc_pri))
		return;

	edmav2_rx_process_last_segment(rxdesc_ring, rxdesc_pri);
}

static void edmav2_rx_handle_linear_packet(struct edmav2_rxdesc_ring *rxdesc_ring,
					   struct edmav2_rxdesc_pri *rxdesc_pri,
					   struct sk_buff *skb, int src_port)
{
	skb_put(skb, EDMAV2_RXDESC_PACKET_LEN_GET(rxdesc_pri));
	__skb_pull(skb, EDMAV2_RXDESC_DATA_OFFSET_GET(rxdesc_pri));

	edmav2_rx_deliver(rxdesc_ring, rxdesc_pri, skb, src_port);
}

static int edmav2_rx_reap(struct edmav2_rxdesc_ring *rxdesc_ring, int budget)
{
	struct edmav2 *priv = rxdesc_ring->priv;
	u32 alloc_size = rxdesc_ring->rxfill->alloc_size;
	struct ppe_device *ppe_dev = priv->ppe_dev;
	u32 idx_mask = priv->hw_info->idx_mask;
	struct edmav2_rxdesc_pri *next_rxdesc_pri;
	struct regmap *regmap = ppe_dev->regmap;
	struct device *dev = ppe_dev->dev;
	u32 prod_idx, cons_idx;
	u32 work_to_do, work_done = 0;
	struct sk_buff *next_skb;
	u32 work_leftover, reg;

	/* Get Rx ring producer and consumer indices. */
	cons_idx = rxdesc_ring->cons_idx;

	if (likely(rxdesc_ring->work_leftover > EDMAV2_RX_MAX_PROCESS)) {
		work_to_do = rxdesc_ring->work_leftover;
	} else {
		reg = EDMA_BASE_OFFSET + EDMAV2_REG_RXDESC_PROD_IDX(rxdesc_ring->ring_id);
		regmap_read(regmap, reg, &prod_idx);

		prod_idx = prod_idx & idx_mask;

		work_to_do = EDMAV2_DESC_AVAIL_COUNT(prod_idx,
						     cons_idx, EDMAV2_RX_RING_SIZE);
		rxdesc_ring->work_leftover = work_to_do;
	}

	if (work_to_do > budget)
		work_to_do = budget;

	rxdesc_ring->work_leftover -= work_to_do;
	next_rxdesc_pri = EDMAV2_RXDESC_PRI_DESC(rxdesc_ring, cons_idx);

	/* Get opaque from RXDESC. */
	next_skb = (struct sk_buff *)EDMAV2_RXDESC_OPAQUE_GET(next_rxdesc_pri);

	work_leftover = work_to_do & (EDMAV2_RX_MAX_PROCESS - 1);
	while (likely(work_to_do--)) {
		struct edmav2_rxdesc_pri *rxdesc_pri;
		struct sk_buff *skb;
		dma_addr_t dma_addr;
		u32 dma_map_size;
		int src_port;

		skb = next_skb;
		rxdesc_pri = next_rxdesc_pri;
		dma_addr = EDMAV2_RXDESC_BUFFER_ADDR_GET(rxdesc_pri);
		dma_map_size = alloc_size - EDMAV2_RX_SKB_HEADROOM - NET_IP_ALIGN;

		dma_unmap_single(dev, dma_addr, dma_map_size, DMA_FROM_DEVICE);

		/* Update consumer index. */
		cons_idx = (cons_idx + 1) & EDMAV2_RX_RING_SIZE_MASK;

		/* Get the next Rx descriptor. */
		next_rxdesc_pri = EDMAV2_RXDESC_PRI_DESC(rxdesc_ring, cons_idx);
		next_skb = (struct sk_buff *)EDMAV2_RXDESC_OPAQUE_GET(next_rxdesc_pri);

		if (likely(!rxdesc_ring->head)) {
			/* First segment of a frame. */
			src_port = edmav2_rx_src_port(rxdesc_ring, rxdesc_pri);
			if (unlikely(src_port < 0)) {
				dev_kfree_skb_any(skb);
				goto next_rx_desc;
			}

			/* Handle linear packets. */
			if (likely(!EDMAV2_RXDESC_MORE_BIT_GET(rxdesc_pri))) {
				edmav2_rx_handle_linear_packet(rxdesc_ring, rxdesc_pri,
							       skb, src_port);
				goto next_rx_desc;
			}
		} else {
			src_port = rxdesc_ring->src_port;
		}

		/* Handle scatter frame processing for first/middle/last segments. */
		edmav2_rx_handle_frag_list(rxdesc_ring, rxdesc_pri, skb, src_port);

next_rx_desc:
		/* Update work done. */
		work_done++;

		/* Check if we can refill EDMAV2_RX_MAX_PROCESS worth buffers,
		 * if yes, refill and update index before continuing.
		 */
		if (unlikely(!(work_done & (EDMAV2_RX_MAX_PROCESS - 1)))) {
			reg = EDMA_BASE_OFFSET + EDMAV2_REG_RXDESC_CONS_IDX(rxdesc_ring->ring_id);
			regmap_write(regmap, reg, cons_idx);
			rxdesc_ring->cons_idx = cons_idx;
			edmav2_rx_alloc_buffer_list(rxdesc_ring->rxfill, EDMAV2_RX_MAX_PROCESS);
		}
	}

	/* Check if we need to refill and update
	 * index for any buffers before exit.
	 */
	if (unlikely(work_leftover)) {
		reg = EDMA_BASE_OFFSET + EDMAV2_REG_RXDESC_CONS_IDX(rxdesc_ring->ring_id);
		regmap_write(regmap, reg, cons_idx);
		rxdesc_ring->cons_idx = cons_idx;
		edmav2_rx_alloc_buffer_list(rxdesc_ring->rxfill, work_leftover);
	}

	return work_done;
}

/**
 * edmav2_rx_napi_poll - EDMA Rx napi poll.
 * @napi: NAPI structure
 * @budget: Rx NAPI budget
 *
 * EDMA RX NAPI handler to handle the NAPI poll.
 *
 * Return the number of packets processed.
 */
int edmav2_rx_napi_poll(struct napi_struct *napi, int budget)
{
	struct edmav2_rxdesc_ring *rxdesc_ring =
		container_of(napi, struct edmav2_rxdesc_ring, napi);
	struct edmav2 *priv = rxdesc_ring->priv;
	struct ppe_device *ppe_dev = priv->ppe_dev;
	struct regmap *regmap = ppe_dev->regmap;
	int work_done = 0;
	u32 status, reg;

	do {
		work_done += edmav2_rx_reap(rxdesc_ring, budget - work_done);
		if (likely(work_done >= budget))
			return work_done;

		/* Check if there are more packets to process. */
		reg = EDMA_BASE_OFFSET + EDMAV2_REG_RXDESC_INT_STAT(rxdesc_ring->ring_id);
		regmap_read(regmap, reg, &status);
		status = status & EDMAV2_RXDESC_RING_INT_STATUS_MASK;
	} while (likely(status));

	napi_complete(napi);

	reg = EDMA_BASE_OFFSET + EDMAV2_REG_RXDESC_INT_MASK(rxdesc_ring->ring_id);
	regmap_write(regmap, reg, priv->intr_info.intr_mask_rx);

	return work_done;
}

/**
 * edmav2_rx_handle_irq - EDMA Rx handle irq.
 * @irq: Interrupt to handle
 * @ctx: Context
 *
 * Process RX IRQ and schedule NAPI.
 *
 * Return IRQ_HANDLED(1) on success.
 */
irqreturn_t edmav2_rx_handle_irq(int irq, void *ctx)
{
	struct edmav2_rxdesc_ring *rxdesc_ring = ctx;
	struct edmav2 *priv = rxdesc_ring->priv;
	struct ppe_device *ppe_dev = priv->ppe_dev;
	struct regmap *regmap = ppe_dev->regmap;
	u32 reg;

	if (likely(napi_schedule_prep(&rxdesc_ring->napi))) {
		/* Disable RxDesc interrupt. */
		reg = EDMA_BASE_OFFSET + EDMAV2_REG_RXDESC_INT_MASK(rxdesc_ring->ring_id);
		regmap_write(regmap, reg, EDMAV2_MASK_INT_DISABLE);
		__napi_schedule(&rxdesc_ring->napi);
	}

	return IRQ_HANDLED;
}
