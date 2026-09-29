/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

/* EDMA v1, the IPQ6018 and IPQ8074 generation. A frame has a 2-word
 * descriptor and a software preheader at the start of its buffer.
 */

#ifndef __EDMAV1_EDMA_H__
#define __EDMAV1_EDMA_H__

#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/netdevice.h>

#include "../edma.h"

/* Global / control registers */
#define EDMAV1_REG_PORT_CTRL 0x4
#define EDMAV1_REG_TXDESC2CMPL_MAP_0 0xc
#define EDMAV1_REG_RXDESC2FILL_MAP_0 0x18
#define EDMAV1_REG_RXDESC2FILL_MAP_1 0x1c
#define EDMAV1_REG_DMAR_CTRL 0x48
#define EDMAV1_REG_AXIW_CTRL 0x50
#define EDMAV1_REG_MISC_INT_STAT 0x5c
#define EDMAV1_REG_MISC_INT_MASK 0x60

#define EDMAV1_PORT_PAD_EN 0x1
#define EDMAV1_PORT_EDMA_EN 0x2

#define EDMAV1_DMAR_REQ_PRI_MASK 0x7
#define EDMAV1_DMAR_REQ_PRI_SHIFT 0
#define EDMAV1_DMAR_BURST_LEN_MASK 0x1
#define EDMAV1_DMAR_BURST_LEN_SHIFT 3
#define EDMAV1_DMAR_TXDATA_NUM_MASK 0x1f
#define EDMAV1_DMAR_TXDATA_NUM_SHIFT 4
#define EDMAV1_DMAR_TXDESC_NUM_MASK 0x7
#define EDMAV1_DMAR_TXDESC_NUM_SHIFT 9
#define EDMAV1_DMAR_RXFILL_NUM_MASK 0x7
#define EDMAV1_DMAR_RXFILL_NUM_SHIFT 12

#define EDMAV1_DMAR_REQ_PRI_SET(x) \
	(((x) & EDMAV1_DMAR_REQ_PRI_MASK) << EDMAV1_DMAR_REQ_PRI_SHIFT)
#define EDMAV1_DMAR_TXDATA_NUM_SET(x) \
	(((x) & EDMAV1_DMAR_TXDATA_NUM_MASK) << EDMAV1_DMAR_TXDATA_NUM_SHIFT)
#define EDMAV1_DMAR_TXDESC_NUM_SET(x) \
	(((x) & EDMAV1_DMAR_TXDESC_NUM_MASK) << EDMAV1_DMAR_TXDESC_NUM_SHIFT)
#define EDMAV1_DMAR_RXFILL_NUM_SET(x) \
	(((x) & EDMAV1_DMAR_RXFILL_NUM_MASK) << EDMAV1_DMAR_RXFILL_NUM_SHIFT)
#define EDMAV1_DMAR_BURST_LEN_SET(x) \
	(((x) & EDMAV1_DMAR_BURST_LEN_MASK) << EDMAV1_DMAR_BURST_LEN_SHIFT)

#define EDMAV1_AXIW_MAX_WR_SIZE_EN 0x400

/* Registers that the register dump reports, as offset and value pairs. */
#define EDMAV1_REGS_COUNT 34

/* Sizes and ring configuration */
#define EDMAV1_MAX_FRAME_SIZE 12288
#define EDMAV1_MAX_MTU (EDMAV1_MAX_FRAME_SIZE - ETH_HLEN - ETH_FCS_LEN - \
		      (2 * VLAN_HLEN))

/* The frame size that the receive buffers are built for until the port model
 * asks for another.
 */
#define EDMAV1_DEFAULT_FRAME_SIZE (ETH_DATA_LEN + ETH_HLEN + 2 * VLAN_HLEN)

struct edmav1_soc_data {
	u32 txcmpl_base;
	u32 tx_int_base;
	u32 misc_int_mask;
	u8 txdesc_ring;
	u8 txcmpl_ring;
	u8 rxfill_ring;
	u8 rxdesc_ring;
	u8 tx_min_size;
	bool txdesc2cmpl_map;
	bool burst_enable;
	bool axiw_enable;
};

struct edmav1_ring {
	void *desc;
	dma_addr_t dma;
	u16 count;
	struct sk_buff **skb_store;
	struct page **page_store;
};

#include "rx.h"
#include "tx.h"

struct edmav1_stats {
	u64 rx_untracked_page;
	u64 rx_bad_src_info;
	u64 rx_no_skb;
	u64 rx_split_frame;
	u64 rx_fill_starved;
	u64 tx_desc_error;
	u64 tx_unnamed_frame;
	u64 misc_error;
};

struct edmav1 {
	struct edma *edma;
	const struct edmav1_soc_data *soc;
	u32 rx_buffer_size;
	u8 rx_page_order;

	struct edmav1_rx_queue rxq[EDMAV1_NUM_RX_QUEUES_MAX];
	/* Number of rxq[] entries in use. Queue 0 is mandatory and the others
	 * are used only if their named interrupt exists.
	 */
	int num_rx_queues;

	struct edmav1_tx_queue txq[EDMAV1_NUM_TX_QUEUES_MAX];
	/* Same for TX. */
	int num_tx_queues;

	/* Counted here because the netdev statistics cannot say which of the
	 * reasons a frame went missing.
	 */
	struct edmav1_stats stats;

	/* Only rxq[0] has an interrupt for its rxfill ring. The other queues
	 * refill at the end of their poll.
	 */
	int rxfill_irq;
	int misc_irq;
};

/* Counted on the netdev that serves the conduit, if there is one. */
#define EDMAV1_DEV_STATS_INC(priv, field)				\
do {									\
	struct net_device *__netdev = READ_ONCE((priv)->edma->netdev[0]);	\
									\
	if (__netdev)							\
		DEV_STATS_INC(__netdev, field);				\
} while (0)

int edmav1_ring_alloc(struct edmav1 *priv, struct edmav1_ring *ring, int count,
		      int desc_size);
void edmav1_ring_free(struct edmav1 *priv, struct edmav1_ring *ring,
		      int desc_size);
void edmav1_hw_stop(struct edmav1 *priv);
int edmav1_hw_init(struct edmav1 *priv);

/* The implementation of the EDMA API. */
int edmav1_init(struct edma *edma);
void edmav1_fini(struct edma *edma);
void edmav1_open(struct edma *edma);
void edmav1_close(struct edma *edma);
netdev_tx_t edmav1_xmit(struct edma *edma, struct sk_buff *skb, u8 dst_port,
			u8 txq);
int edmav1_set_max_frame(struct edma *edma, unsigned int frame_size);
const struct edma_stat_desc *edmav1_stats_layout(struct edma *edma,
						 unsigned int *count);
void edmav1_stats_read(struct edma *edma, u64 *buf);
int edmav1_ringparam_get(struct edma *edma, struct ethtool_ringparam *rp);
int edmav1_regs_len(struct edma *edma);
void edmav1_regs_dump(struct edma *edma, void *buf);

#endif /* __EDMAV1_EDMA_H__ */
