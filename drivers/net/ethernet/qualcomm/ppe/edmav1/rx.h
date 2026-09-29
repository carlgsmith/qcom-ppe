/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __EDMAV1_RX_H__
#define __EDMAV1_RX_H__

#include <linux/interrupt.h>
#include <linux/netdevice.h>
#include <net/page_pool/helpers.h>

struct edmav1;

/* RX fill ring registers */
#define EDMAV1_REG_RXFILL_BA(n) (0x29000 + (0x1000 * (n)))
#define EDMAV1_REG_RXFILL_PROD_IDX(n) (0x29004 + (0x1000 * (n)))
#define EDMAV1_REG_RXFILL_CONS_IDX(n) (0x29008 + (0x1000 * (n)))
#define EDMAV1_REG_RXFILL_RING_SIZE(n) (0x2900c + (0x1000 * (n)))
#define EDMAV1_REG_RXFILL_RING_EN(n) (0x2901c + (0x1000 * (n)))

#define EDMAV1_RXFILL_PROD_IDX_MASK 0xffff
#define EDMAV1_RXFILL_CONS_IDX_MASK 0xffff
#define EDMAV1_RXFILL_RING_SIZE_MASK 0xffff
#define EDMAV1_RXFILL_BUF_SIZE_MASK 0x3fff
#define EDMAV1_RXFILL_RING_EN 0x1
#define EDMAV1_RXFILL_INT_MASK 0x1

/* RX fill ring interrupt registers */
#define EDMAV1_REG_RXFILL_INT_STAT(n) (0x31000 + (0x1000 * (n)))
#define EDMAV1_REG_RXFILL_INT_MASK(n) (0x31004 + (0x1000 * (n)))

/* RX descriptor ring registers */
#define EDMAV1_REG_RXDESC_BA(n) (0x39000 + (0x1000 * (n)))
#define EDMAV1_REG_RXDESC_PROD_IDX(n) (0x39004 + (0x1000 * (n)))
#define EDMAV1_REG_RXDESC_CONS_IDX(n) (0x39008 + (0x1000 * (n)))
#define EDMAV1_REG_RXDESC_RING_SIZE(n) (0x3900c + (0x1000 * (n)))
#define EDMAV1_REG_RXDESC_CTRL(n) (0x39018 + (0x1000 * (n)))

#define EDMAV1_RXDESC_PROD_IDX_MASK 0xffff
#define EDMAV1_RXDESC_CONS_IDX_MASK 0xffff
#define EDMAV1_RXDESC_RING_SIZE_MASK 0xffff
#define EDMAV1_RXDESC_PL_OFFSET_MASK 0x1ff
#define EDMAV1_RXDESC_PL_OFFSET_SHIFT 16
#define EDMAV1_RXDESC_RX_EN 0x1
#define EDMAV1_RXDESC_PACKET_LEN_MASK 0x3fff
/* A frame the engine had to spread over several receive descriptors. The fill
 * buffers are sized for the largest frame the conduit accepts, so it does not
 * arise; a frame that carried it would be handed up in pieces.
 */
#define EDMAV1_RXDESC_MORE BIT(30)

/* RX descriptor ring interrupt registers */
#define EDMAV1_REG_RXDESC_INT_STAT(n) (0x49000 + (0x1000 * (n)))
#define EDMAV1_REG_RXDESC_INT_MASK(n) (0x49004 + (0x1000 * (n)))
#define EDMAV1_REG_RX_MOD_TIMER(n) (0x49008 + (0x1000 * (n)))
#define EDMAV1_REG_RX_INT_CTRL(n) (0x4900c + (0x1000 * (n)))

#define EDMAV1_RXDESC_INT_MASK_PKT_INT 0x1
#define EDMAV1_RX_MOD_TIMER_INIT 1000

/* PPE queue to receive ring mapping: a four-bit ring id per switch queue,
 * eight packed per 32-bit register.
 */
#define EDMAV1_QID2RID_TABLE_MEM(n) (0x5a000 + (0x4 * (n)))
#define EDMAV1_QID2RID_DEPTH 0x40
#define EDMAV1_QID2RID_RING_MASK 0xf

#define EDMAV1_RX_RING_SIZE 2048

struct edmav1_rxdesc {
	u32 buffer_addr;
	u32 status;
};

struct edmav1_rxfill_desc {
	u32 buffer_addr;
	u32 word1;
};

struct edmav1_rx_preheader {
	u32 opaque;
	u16 src_info;
	u16 dst_info;
	u32 rx_pre2;
	u32 rx_pre3;
	u32 rx_pre4;
	u32 rx_pre5;
	u32 rx_pre6;
	u32 rx_pre7;
};

#define EDMAV1_RX_PREHDR_SIZE (sizeof(struct edmav1_rx_preheader))

/* Descriptor accessors */
#define EDMAV1_RX_GET_DESC(R, i, type) (&(((type *)((R)->desc))[i]))
#define EDMAV1_RXFILL_DESC(R, i) EDMAV1_RX_GET_DESC(R, i, struct edmav1_rxfill_desc)
#define EDMAV1_RXDESC_DESC(R, i) EDMAV1_RX_GET_DESC(R, i, struct edmav1_rxdesc)

/* Preheader fields */
#define EDMAV1_DST_PORT_TYPE 0x20
#define EDMAV1_DST_PORT_ID_MASK 0x1f
#define EDMAV1_SRC_PORT_MASK 0x0fff
#define EDMAV1_PREHDR_DSTINFO_PORTID_IND 0x20
#define EDMAV1_RXPH_SRC_INFO_TYPE_GET(rxph) (((rxph)->src_info >> 8) & 0xf0)

/* RX preheader hash fields, used by edmav1_rx_hash(). */
#define EDMAV1_RXPH_HASH_MASK GENMASK(20, 0)
#define EDMAV1_RXPH_HASH_FLAG_SHIFT 21
#define EDMAV1_RXPH_HASH_FLAG_MASK 0x7
#define EDMAV1_RXPH_HASH_5TUPLE 1
#define EDMAV1_RXPH_HASH_3TUPLE 2

/* Up to 4 RX queues, one per CPU. The PPE spreads flows over them by hash, so
 * the number of queues must match what the port model programs into the PPE
 * (edma_caps.rx_queues). The ring indices count down from the highest ring of
 * the SoC (soc->rxdesc_ring - i and soc->rxfill_ring - i), because the low
 * rings belong to the NSS firmware and raise no interrupts.
 */
#define EDMAV1_NUM_RX_QUEUES_MAX 4

struct edmav1_rx_queue {
	struct edmav1 *priv;
	struct napi_struct napi;
	struct edmav1_ring rxdesc_ring;
	struct edmav1_ring rxfill_ring;
	struct page_pool *page_pool;
	u8 ring_index;
	u8 rxfill_ring_index;
	/* The rxdesc interrupt of this queue. It is the only thing that
	 * schedules the NAPI of the queue.
	 */
	int irq;
};

irqreturn_t edmav1_rxdesc_irq_handle(int irq, void *ctx);
irqreturn_t edmav1_rxfill_irq_handle(int irq, void *ctx);
int edmav1_rx_fill(struct edmav1_rx_queue *rxq);
bool edmav1_rx_page_take(struct edmav1_rx_queue *rxq, struct page *page, u32 store_idx);
int edmav1_rx_napi(struct napi_struct *napi, int budget);

#endif /* __EDMAV1_RX_H__ */
