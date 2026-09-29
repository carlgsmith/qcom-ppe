/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __EDMAV1_TX_H__
#define __EDMAV1_TX_H__

#include <linux/interrupt.h>
#include <linux/netdevice.h>

struct edmav1;

/* TX descriptor ring registers */
#define EDMAV1_REG_TXDESC_BA(n) (0x1000 + (0x1000 * (n)))
#define EDMAV1_REG_TXDESC_PROD_IDX(n) (0x1004 + (0x1000 * (n)))
#define EDMAV1_REG_TXDESC_CONS_IDX(n) (0x1008 + (0x1000 * (n)))
#define EDMAV1_REG_TXDESC_RING_SIZE(n) (0x100c + (0x1000 * (n)))
#define EDMAV1_REG_TXDESC_CTRL(n) (0x1010 + (0x1000 * (n)))

#define EDMAV1_TXDESC_PROD_IDX_MASK 0xffff
#define EDMAV1_TXDESC_CONS_IDX_MASK 0xffff
#define EDMAV1_TXDESC_RING_SIZE_MASK 0xffff
#define EDMAV1_TXDESC_TX_EN 0x1

/* TX completion ring registers */
#define EDMAV1_REG_TXCMPL_BA(b, n)        ((b) + (0x1000 * (n)))
#define EDMAV1_REG_TXCMPL_PROD_IDX(b, n)  ((b) + 0x004 + (0x1000 * (n)))
#define EDMAV1_REG_TXCMPL_CONS_IDX(b, n)  ((b) + 0x008 + (0x1000 * (n)))
#define EDMAV1_REG_TXCMPL_RING_SIZE(b, n) ((b) + 0x00c + (0x1000 * (n)))
#define EDMAV1_REG_TXCMPL_CTRL(b, n)      ((b) + 0x014 + (0x1000 * (n)))

#define EDMAV1_TXCMPL_PROD_IDX_MASK 0xffff
#define EDMAV1_TXCMPL_CONS_IDX_MASK 0xffff
#define EDMAV1_TXCMPL_RETMODE_OPAQUE 0x0
/* The engine returns one completion per descriptor: the more bit marks every
 * completion of a frame but its last, and the error field reports what the
 * engine made of the descriptor, above the ring id it read it from.
 */
#define EDMAV1_TXCMPL_ERROR GENMASK(29, 7)
#define EDMAV1_TXCMPL_MORE BIT(30)

/* TX interrupt registers */
#define EDMAV1_REG_TX_INT_STAT(b, n)  ((b) + (0x1000 * (n)))
#define EDMAV1_REG_TX_INT_MASK(b, n)  ((b) + 0x004 + (0x1000 * (n)))
#define EDMAV1_REG_TX_MOD_TIMER(b, n) ((b) + 0x008 + (0x1000 * (n)))
#define EDMAV1_REG_TX_INT_CTRL(b, n)  ((b) + 0x00c + (0x1000 * (n)))

#define EDMAV1_TX_INT_MASK 0x3
#define EDMAV1_TX_MOD_TIMER 150

/* TXDESC to TXCMPL ring mapping */
#define EDMAV1_REG_TXDESC2CMPL_MAP(n) (0x0c + 0x4 * (n))

#define EDMAV1_TX_RING_SIZE 128
/* The bounds a frame is described to the engine within: at most this many
 * buffers, and no buffer below this size before the last. The queue stops
 * with room for a frame that takes every descriptor, or it would restart on
 * space that the next frame still could not use.
 */
#define EDMAV1_TX_MAX_SEGS 32
#define EDMAV1_TX_MIN_SEG 16
#define EDMAV1_TX_RING_THRESH (EDMAV1_TX_MAX_SEGS + 1)

struct edmav1_txdesc {
	u32 buffer_addr;
	u32 word1;
};

struct edmav1_txcmpl {
	u32 buffer_addr;
	u32 status;
};

struct edmav1_tx_preheader {
	u32 opaque;
	u16 src_info;
	u16 dst_info;
	u32 tx_pre2;
	u32 tx_pre3;
	u32 tx_pre4;
	u32 tx_pre5;
	u32 tx_pre6;
	u32 tx_pre7;
};

#define EDMAV1_TX_PREHDR_SIZE (sizeof(struct edmav1_tx_preheader))

/* Descriptor accessors */
#define EDMAV1_TX_GET_DESC(R, i, type) (&(((type *)((R)->desc))[i]))
#define EDMAV1_TXDESC_DESC(R, i) EDMAV1_TX_GET_DESC(R, i, struct edmav1_txdesc)
#define EDMAV1_TXCMPL_DESC(R, i) EDMAV1_TX_GET_DESC(R, i, struct edmav1_txcmpl)

/* TX descriptor fields */
#define EDMAV1_TXDESC_MORE BIT(30)
#define EDMAV1_TXDESC_TSO_EN BIT(28)
#define EDMAV1_TXDESC_PREHEADER_SHIFT 29
#define EDMAV1_TXDESC_PREHEADER BIT(EDMAV1_TXDESC_PREHEADER_SHIFT)
#define EDMAV1_TXDESC_DATA_OFFSET_SHIFT 16
#define EDMAV1_TXDESC_DATA_OFFSET_MASK 0xff
#define EDMAV1_TXDESC_DATA_LENGTH_MASK 0xffff

/* TX preheader fields */
#define EDMAV1_TX_PRE4_ADV_OFFLOAD_EN BIT(28)
#define EDMAV1_TX_PRE6_CSUM_MODE_L4 (0x1 << 29)
#define EDMAV1_TX_PRE6_MSS_MASK 0x3fff
#define EDMAV1_TX_PRE6_IP_CSUM_EN BIT(31)

/* Up to 4 TX queues, one per CPU. The port model picks the queue with the
 * number of the current CPU, and the PPE takes no part in it. The index in
 * txq[] must be the CPU that core_affinity_edma() assigns to the ring. The
 * txcmpl group of the device tree lists the mandatory ring first (CPU0) and
 * the other rings in device tree order (CPU i).
 */
#define EDMAV1_NUM_TX_QUEUES_MAX 4

struct edmav1_tx_queue {
	struct edmav1 *priv;
	struct napi_struct napi;
	struct edmav1_ring txdesc_ring;
	struct edmav1_ring txcmpl_ring;
	/* Serialises the transmit of the queue. */
	spinlock_t tx_lock;
	/* The frame a run of completions belongs to, named by the first of
	 * them and released on the last.
	 */
	struct sk_buff *txcmpl_skb;
	u32 txcmpl_idx;
	bool txcmpl_run;
	/* The txcmpl interrupt of this queue. It is the only thing that
	 * schedules the NAPI of the queue.
	 */
	int irq;
	/* The number of the txdesc ring and the number of the txcmpl ring. They
	 * are the same on some SoCs. The others map the txdesc ring to its
	 * txcmpl ring with TXDESC2CMPL_MAP.
	 */
	u8 ring_index;
	u8 cmpl_ring_index;
};

irqreturn_t edmav1_tx_irq_handle(int irq, void *ctx);
u32 edmav1_clean_tx(struct edmav1_tx_queue *txq, int budget, int napi_budget);
int edmav1_tx_napi(struct napi_struct *napi, int budget);
u16 edmav1_txdesc_free(struct edmav1_tx_queue *txq);
u32 edmav1_tx_release(struct edmav1_tx_queue *txq, u32 idx, struct sk_buff *skb,
		      int napi_budget);

#endif /* __EDMAV1_TX_H__ */
