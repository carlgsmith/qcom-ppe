/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __EDMAV2_TX_H__
#define __EDMAV2_TX_H__

#include <linux/netdevice.h>
#include <linux/u64_stats_sync.h>

struct edma;
struct edmav2;

#define EDMAV2_GET_DESC(R, i, type)	(&(((type *)((R)->desc))[(i)]))
#define EDMAV2_GET_PDESC(R, i, type)	(&(((type *)((R)->pdesc))[(i)]))
#define EDMAV2_GET_SDESC(R, i, type)	(&(((type *)((R)->sdesc))[(i)]))
#define EDMAV2_TXCMPL_DESC(R, i)		EDMAV2_GET_DESC(R, i, \
						struct edmav2_txcmpl_desc)
#define EDMAV2_TXDESC_PRI_DESC(R, i)	EDMAV2_GET_PDESC(R, i, \
						struct edmav2_txdesc_pri)
#define EDMAV2_TXDESC_SEC_DESC(R, i)	EDMAV2_GET_SDESC(R, i, \
						struct edmav2_txdesc_sec)

#define EDMAV2_DESC_AVAIL_COUNT(head, tail, _max) ({ \
			typeof(_max) (max) = (_max); \
			((((head) - (tail)) + \
			(max)) & ((max) - 1)); })

#define EDMAV2_TX_RING_SIZE               2048
#define EDMAV2_TX_RING_SIZE_MASK		(EDMAV2_TX_RING_SIZE - 1)

/* HW defined low and high MSS size. */
#define EDMAV2_TX_TSO_MSS_MIN		256
#define EDMAV2_TX_TSO_MSS_MAX		10240

#define EDMAV2_DST_PORT_TYPE		2
#define EDMAV2_DST_PORT_TYPE_SHIFT	28
#define EDMAV2_DST_PORT_TYPE_MASK		(0xf << EDMAV2_DST_PORT_TYPE_SHIFT)
#define EDMAV2_DST_PORT_ID_SHIFT		16
#define EDMAV2_DST_PORT_ID_MASK		(0xfff << EDMAV2_DST_PORT_ID_SHIFT)

#define EDMAV2_DST_PORT_TYPE_SET(x)	(((x) << EDMAV2_DST_PORT_TYPE_SHIFT) & \
							EDMAV2_DST_PORT_TYPE_MASK)
#define EDMAV2_DST_PORT_ID_SET(x)		(((x) << EDMAV2_DST_PORT_ID_SHIFT) & \
							EDMAV2_DST_PORT_ID_MASK)
#define EDMAV2_DST_INFO_SET(desc, x)	((desc)->word4 |= \
	(EDMAV2_DST_PORT_TYPE_SET(EDMAV2_DST_PORT_TYPE) | EDMAV2_DST_PORT_ID_SET(x)))

#define EDMAV2_TXDESC_TSO_ENABLE_MASK		BIT(24)
#define EDMAV2_TXDESC_TSO_ENABLE_SET(desc, x)	((desc)->word5 |= \
				FIELD_PREP(EDMAV2_TXDESC_TSO_ENABLE_MASK, x))
#define EDMAV2_TXDESC_MSS_MASK			GENMASK(31, 16)
#define EDMAV2_TXDESC_MSS_SET(desc, x)		((desc)->word6 |= \
					FIELD_PREP(EDMAV2_TXDESC_MSS_MASK, x))
#define EDMAV2_TXDESC_MORE_BIT_MASK	BIT(30)
#define EDMAV2_TXDESC_MORE_BIT_SET(desc, x)	((desc)->word1 |= \
				FIELD_PREP(EDMAV2_TXDESC_MORE_BIT_MASK, x))

#define EDMAV2_TXDESC_ADV_OFFSET_BIT	BIT(31)
#define EDMAV2_TXDESC_ADV_OFFLOAD_SET(desc)	((desc)->word5 |= \
					FIELD_PREP(EDMAV2_TXDESC_ADV_OFFSET_BIT, 1))
#define EDMAV2_TXDESC_IP_CSUM_BIT		BIT(25)
#define EDMAV2_TXDESC_IP_CSUM_SET(desc)		((desc)->word5 |= \
					FIELD_PREP(EDMAV2_TXDESC_IP_CSUM_BIT, 1))

#define EDMAV2_TXDESC_L4_CSUM_SET_MASK   GENMASK(27, 26)
#define EDMAV2_TXDESC_L4_CSUM_SET(desc)  ((desc)->word5 |= \
			       (FIELD_PREP(EDMAV2_TXDESC_L4_CSUM_SET_MASK, 1)))

#define EDMAV2_TXDESC_DATA_LEN_SET(desc, x)	((desc)->word5 |= ((x) & 0x1ffff))
#define EDMAV2_TXDESC_SERVICE_CODE_MASK	GENMASK(24, 16)
#define EDMAV2_TXDESC_SERVICE_CODE_SET(desc, x)	((desc)->word1 |= \
				(FIELD_PREP(EDMAV2_TXDESC_SERVICE_CODE_MASK, x)))
#define EDMAV2_TXDESC_BUFFER_ADDR_SET(desc, addr)	(((desc)->word0) = (addr))

#ifdef __LP64__
#define EDMAV2_TXDESC_OPAQUE_GET(_desc) ({ \
			typeof(_desc) (desc) = (_desc); \
			(((u64)(desc)->word3 << 32) | (desc)->word2); })

#define EDMAV2_TXCMPL_OPAQUE_GET(_desc) ({ \
			typeof(_desc) (desc) = (_desc); \
			(((u64)(desc)->word1 << 32) | \
			(desc)->word0); })

#define EDMAV2_TXDESC_OPAQUE_LO_SET(desc, ptr)	((desc)->word2 = \
						(u32)(uintptr_t)(ptr))

#define EDMAV2_TXDESC_OPAQUE_HI_SET(desc, ptr)	((desc)->word3 = \
						(u32)((u64)(ptr) >> 32))

#define EDMAV2_TXDESC_OPAQUE_SET(_desc, _ptr)	do { \
	typeof(_desc) (desc) = (_desc); \
	typeof(_ptr) (ptr) = (_ptr); \
	EDMAV2_TXDESC_OPAQUE_LO_SET(desc, ptr); \
	EDMAV2_TXDESC_OPAQUE_HI_SET(desc, ptr); \
} while (0)
#else
#define EDMAV2_TXCMPL_OPAQUE_GET(desc)		((desc)->word0)
#define EDMAV2_TXDESC_OPAQUE_GET(desc)		((desc)->word2)
#define EDMAV2_TXDESC_OPAQUE_LO_SET(desc, ptr)	((desc)->word2 = (u32)(uintptr_t)ptr)

#define EDMAV2_TXDESC_OPAQUE_SET(desc, ptr)	\
					EDMAV2_TXDESC_OPAQUE_LO_SET(desc, ptr)
#endif
#define EDMAV2_TXCMPL_MORE_BIT_MASK	BIT(30)

#define EDMAV2_TXCMPL_MORE_BIT_GET(desc)	((le32_to_cpu((__force __le32)((desc)->word2))) & \
					EDMAV2_TXCMPL_MORE_BIT_MASK)

#define EDMAV2_TXCOMP_RING_ERROR_MASK	GENMASK(22, 0)

#define EDMAV2_TXCOMP_RING_ERROR_GET(x)	((le32_to_cpu((__force __le32)x)) & \
					EDMAV2_TXCOMP_RING_ERROR_MASK)

/* Opaque values are set in word2 and word3,
 * they are not accessed by the EDMA HW,
 * so endianness conversion is not needed.
 */
#define EDMAV2_TXDESC_ENDIAN_SET(_desc)	({ \
	typeof(_desc) (desc) = (_desc); \
	cpu_to_le32s(&((desc)->word0)); \
	cpu_to_le32s(&((desc)->word1)); \
	cpu_to_le32s(&((desc)->word4)); \
	cpu_to_le32s(&((desc)->word5)); \
	cpu_to_le32s(&((desc)->word6)); \
	cpu_to_le32s(&((desc)->word7)); \
})

/* EDMA Tx GSO status */
enum edmav2_tx_status {
	EDMAV2_TX_OK = 0,			/* Tx success. */
	EDMAV2_TX_FAIL_NO_DESC = 1,	/* Not enough descriptors. */
	EDMAV2_TX_FAIL = 2,		/* Tx failure. */
};

/* EDMA TX GSO status */
enum edmav2_tx_gso_status {
	EDMAV2_TX_GSO_NOT_NEEDED = 0,
		/* Packet has segment count less than TX_TSO_SEG_MAX. */
	EDMAV2_TX_GSO_SUCCEED = 1,
		/* GSO Succeed. */
	EDMAV2_TX_GSO_FAIL = 2,
		/* GSO failed, drop the packet. */
};

/**
 * struct edmav2_txcmpl_stats - EDMA Tx complete ring statistics.
 * @invalid_buffer: Invalid buffer address received.
 * @errors: Other Tx complete descriptor errors indicated by the hardware.
 * @desc_with_more_bit: Packet's segment transmit count.
 * @no_pending_desc: No descriptor is pending for processing.
 * @syncp: Synchronization pointer.
 */
struct edmav2_txcmpl_stats {
	u64 invalid_buffer;
	u64 errors;
	u64 desc_with_more_bit;
	u64 no_pending_desc;
	struct u64_stats_sync syncp;
};

/**
 * struct edmav2_txdesc_stats - EDMA Tx descriptor ring statistics.
 * @no_desc_avail: No descriptor available to transmit.
 * @tso_max_seg_exceed: Packets extending EDMAV2_TX_TSO_SEG_MAX segments.
 * @syncp: Synchronization pointer.
 */
struct edmav2_txdesc_stats {
	u64 no_desc_avail;
	u64 tso_max_seg_exceed;
	struct u64_stats_sync syncp;
};

/**
 * struct edmav2_txdesc_pri - EDMA primary Tx descriptor.
 * @word0: Low 32-bit of buffer address.
 * @word1: Buffer recycling, PTP tag flag, PRI valid flag.
 * @word2: Low 32-bit of opaque value.
 * @word3: High 32-bit of opaque value.
 * @word4: Source/Destination port info.
 * @word5: VLAN offload, csum mode, ip_csum_en, tso_en, data len.
 * @word6: MSS/hash_value/PTP tag, data offset.
 * @word7: L4/L3 offset, PROT type, L2 type, CVLAN/SVLAN tag, service code.
 */
struct edmav2_txdesc_pri {
	u32 word0;
	u32 word1;
	u32 word2;
	u32 word3;
	u32 word4;
	u32 word5;
	u32 word6;
	u32 word7;
};

/**
 * struct edmav2_txdesc_sec - EDMA secondary Tx descriptor.
 * @word0: Reserved.
 * @word1: Custom csum offset, payload offset, TTL/NAT action.
 * @word2: NAPT translated port, DSCP value, TTL value.
 * @word3: Flow index value and valid flag.
 * @word4: Reserved.
 * @word5: Reserved.
 * @word6: CVLAN/SVLAN command.
 * @word7: CVLAN/SVLAN tag value.
 */
struct edmav2_txdesc_sec {
	u32 word0;
	u32 word1;
	u32 word2;
	u32 word3;
	u32 word4;
	u32 word5;
	u32 word6;
	u32 word7;
};

/**
 * struct edmav2_txcmpl_desc - EDMA Tx complete descriptor.
 * @word0: Low 32-bit opaque value.
 * @word1: High 32-bit opaque value.
 * @word2: More fragment, transmit ring id, pool id.
 * @word3: Error indications.
 */
struct edmav2_txcmpl_desc {
	u32 word0;
	u32 word1;
	u32 word2;
	u32 word3;
};

/**
 * struct edmav2_tx_buf - Buffer that is mapped for a Tx descriptor.
 * @dma: DMA address
 * @len: Length of the mapping, 0 when the descriptor has no mapping
 * @page: The buffer is a fragment that is mapped as a page
 */
struct edmav2_tx_buf {
	dma_addr_t dma;
	u32 len;
	bool page;
};

/**
 * struct edmav2_txdesc_ring - EDMA Tx descriptor ring
 * @priv: EDMA that owns the ring
 * @bufs: Mapped buffer of each descriptor
 * @prod_idx: Producer index
 * @id: Tx ring number
 * @avail_desc: Number of available descriptor to process
 * @pdesc: Primary descriptor ring virtual address
 * @pdma: Primary descriptor ring physical address
 * @sdesc: Secondary descriptor ring virtual address
 * @tx_desc_stats: Tx descriptor ring statistics
 * @sdma: Secondary descriptor ring physical address
 * @count: Number of descriptors
 * @fc_grp_id: Flow control group ID
 */
struct edmav2_txdesc_ring {
	struct edmav2 *priv;
	struct edmav2_tx_buf *bufs;
	u32 prod_idx;
	u32 id;
	u32 avail_desc;
	struct edmav2_txdesc_pri *pdesc;
	dma_addr_t pdma;
	struct edmav2_txdesc_sec *sdesc;
	struct edmav2_txdesc_stats txdesc_stats;
	dma_addr_t sdma;
	u32 count;
	u8 fc_grp_id;
};

/**
 * struct edmav2_txcmpl_ring - EDMA Tx complete ring
 * @priv: EDMA that owns the ring
 * @napi: NAPI
 * @cons_idx: Consumer index
 * @avail_pkt: Number of available packets to process
 * @desc: Descriptor ring virtual address
 * @id: Txcmpl ring number
 * @tx_cmpl_stats: Tx complete ring statistics
 * @dma: Descriptor ring physical address
 * @count: Number of descriptors in the ring
 * @napi_added: Flag to indicate NAPI add status
 */
struct edmav2_txcmpl_ring {
	struct edmav2 *priv;
	struct napi_struct napi;
	u32 cons_idx;
	u32 avail_pkt;
	struct edmav2_txcmpl_desc *desc;
	u32 id;
	struct edmav2_txcmpl_stats txcmpl_stats;
	dma_addr_t dma;
	u32 count;
	bool napi_added;
};

netdev_tx_t edmav2_xmit(struct edma *edma, struct sk_buff *skb, u8 dst_port,
			u8 txq);
irqreturn_t edmav2_tx_handle_irq(int irq, void *ctx);
int edmav2_tx_napi_poll(struct napi_struct *napi, int budget);

#endif
