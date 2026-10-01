/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __EDMAV2_RX_H__
#define __EDMAV2_RX_H__

#include <linux/netdevice.h>
#include <linux/u64_stats_sync.h>

struct edmav2;

#define EDMAV2_RXFILL_RING_PER_CORE_MAX	1
#define EDMAV2_RXDESC_RING_PER_CORE_MAX	1

/* Max Rx processing without replenishing RxFill ring. */
#define EDMAV2_RX_MAX_PROCESS		32

#define EDMAV2_RX_SKB_HEADROOM		128
#define EDMAV2_RX_QUEUE_START		0
#define EDMAV2_RX_BUFFER_SIZE		1984
#define EDMAV2_MAX_CORE			4

#define EDMAV2_GET_DESC(R, i, type)	(&(((type *)((R)->desc))[(i)]))
#define EDMAV2_GET_PDESC(R, i, type)	(&(((type *)((R)->pdesc))[(i)]))
#define EDMAV2_GET_SDESC(R, i, type)	(&(((type *)((R)->sdesc))[(i)]))
#define EDMAV2_RXFILL_DESC(R, i)		EDMAV2_GET_DESC(R, i, \
						struct edmav2_rxfill_desc)
#define EDMAV2_RXDESC_PRI_DESC(R, i)	EDMAV2_GET_PDESC(R, i, \
						struct edmav2_rxdesc_pri)
#define EDMAV2_RXDESC_SEC_DESC(R, i)	EDMAV2_GET_SDESC(R, i, \
						struct edmav2_rxdesc_sec)

#define EDMAV2_RX_RING_SIZE	2048

#define EDMAV2_RX_RING_SIZE_MASK	(EDMAV2_RX_RING_SIZE - 1)
#define EDMAV2_RX_RING_ID_MASK		0x1F

#define EDMAV2_MAX_PRI_PER_CORE      8
#define EDMAV2_RX_PID_IPV4_MAX		0x3
#define EDMAV2_RX_PID_IPV6		0x4
#define EDMAV2_RX_PID_IS_IPV4(pid)	(!((pid) & (~EDMAV2_RX_PID_IPV4_MAX)))
#define EDMAV2_RX_PID_IS_IPV6(pid)	(!(!((pid) & EDMAV2_RX_PID_IPV6)))

#define EDMAV2_RXDESC_BUFFER_ADDR_GET(desc)	\
				((u32)(le32_to_cpu((__force __le32)((desc)->word0))))
#define EDMAV2_RXDESC_OPAQUE_GET(_desc) ({ \
			typeof(_desc) (desc) = (_desc); \
			((uintptr_t)((u64)((desc)->word2) | \
			((u64)((desc)->word3) << 0x20))); })

#define EDMAV2_RXDESC_SRCINFO_TYPE_PORTID		0x2000
#define EDMAV2_RXDESC_SRCINFO_TYPE_MASK		0xF000
#define EDMAV2_RXDESC_L3CSUM_STATUS_MASK		BIT(13)
#define EDMAV2_RXDESC_L4CSUM_STATUS_MASK		BIT(12)
#define EDMAV2_RXDESC_PORTNUM_BITS		0x0FFF

#define EDMAV2_RXDESC_PACKET_LEN_MASK		0x3FFFF
#define EDMAV2_RXDESC_PACKET_LEN_GET(_desc) ({ \
			typeof(_desc) (desc) = (_desc); \
			((le32_to_cpu((__force __le32)((desc)->word5))) & \
			EDMAV2_RXDESC_PACKET_LEN_MASK); })

#define EDMAV2_RXDESC_MORE_BIT_MASK		0x40000000
#define EDMAV2_RXDESC_MORE_BIT_GET(desc) \
	((le32_to_cpu((__force __le32)((desc)->word1))) & \
	 EDMAV2_RXDESC_MORE_BIT_MASK)
#define EDMAV2_RXDESC_SRC_DST_INFO_GET(desc)	\
				((u32)((le32_to_cpu((__force __le32)((desc)->word4)))))

#define EDMAV2_RXDESC_L3_OFFSET_MASK	GENMASK(23, 16)
#define EDMAV2_RXDESC_L3_OFFSET_GET(desc)	FIELD_GET(EDMAV2_RXDESC_L3_OFFSET_MASK, \
					le32_to_cpu((__force __le32)((desc)->word7)))

#define EDMAV2_RXDESC_PID_MASK		GENMASK(15, 12)
#define EDMAV2_RXDESC_PID_GET(desc)	FIELD_GET(EDMAV2_RXDESC_PID_MASK, \
					le32_to_cpu((__force __le32)((desc)->word7)))

#define EDMAV2_RXDESC_DST_INFO_MASK	GENMASK(31, 16)
#define EDMAV2_RXDESC_DST_INFO_GET(desc)	FIELD_GET(EDMAV2_RXDESC_DST_INFO_MASK, \
					le32_to_cpu((__force __le32)((desc)->word4)))

#define EDMAV2_RXDESC_SRC_INFO_MASK	GENMASK(15, 0)
#define EDMAV2_RXDESC_SRC_INFO_GET(desc)	FIELD_GET(EDMAV2_RXDESC_SRC_INFO_MASK, \
					le32_to_cpu((__force __le32)((desc)->word4)))

#define EDMAV2_RXDESC_PORT_ID_MASK	GENMASK(11, 0)
#define EDMAV2_RXDESC_PORT_ID_GET(x)	FIELD_GET(EDMAV2_RXDESC_PORT_ID_MASK, x)

#define EDMAV2_RXDESC_SRC_PORT_ID_GET(desc)	(EDMAV2_RXDESC_PORT_ID_GET \
						(EDMAV2_RXDESC_SRC_INFO_GET(desc)))
#define EDMAV2_RXDESC_DST_PORT_ID_GET(desc)	(EDMAV2_RXDESC_PORT_ID_GET \
						(EDMAV2_RXDESC_DST_INFO_GET(desc)))

#define EDMAV2_RXDESC_DST_PORT		(0x2 << EDMAV2_RXDESC_PID_SHIFT)

#define EDMAV2_RXDESC_L3CSUM_STATUS_GET(desc)	FIELD_GET(EDMAV2_RXDESC_L3CSUM_STATUS_MASK, \
						le32_to_cpu((__force __le32)(desc)->word6))
#define EDMAV2_RXDESC_L4CSUM_STATUS_GET(desc)	FIELD_GET(EDMAV2_RXDESC_L4CSUM_STATUS_MASK, \
						le32_to_cpu((__force __le32)(desc)->word6))

#define EDMAV2_RXDESC_DATA_OFFSET_MASK		GENMASK(11, 0)
#define EDMAV2_RXDESC_DATA_OFFSET_GET(desc)	FIELD_GET(EDMAV2_RXDESC_DATA_OFFSET_MASK, \
						le32_to_cpu((__force __le32)(desc)->word6))

#define EDMAV2_RXFILL_BUF_SIZE_MASK		0xFFFF
#define EDMAV2_RXFILL_BUF_SIZE_SHIFT		16

/* Opaque values are not accessed by the EDMA HW,
 * so endianness conversion is not needed.
 */

#define EDMAV2_RXFILL_OPAQUE_LO_SET(desc, ptr)	(((desc)->word2) = \
						(u32)(uintptr_t)(ptr))
#ifdef __LP64__
#define EDMAV2_RXFILL_OPAQUE_HI_SET(desc, ptr)	(((desc)->word3) = \
						(u32)((u64)(ptr) >> 0x20))
#endif

#define EDMAV2_RXFILL_OPAQUE_GET(_desc) ({ \
			typeof(_desc) (desc) = (_desc); \
			((uintptr_t)((u64)((desc)->word2) | \
			((u64)((desc)->word3) << 0x20))); })

#define EDMAV2_RXFILL_PACKET_LEN_SET(desc, len)	{ \
	(((desc)->word1) = (u32)((((u32)len) << EDMAV2_RXFILL_BUF_SIZE_SHIFT) & \
						0xFFFF0000)); \
}

#define EDMAV2_RXFILL_BUFFER_ADDR_SET(desc, addr)	(((desc)->word0) = (u32)(addr))

/* Opaque values are set in word2 and word3, they are not accessed by the EDMA HW,
 * so endianness conversion is not needed.
 */
#define EDMAV2_RXFILL_ENDIAN_SET(_desc) ({ \
	typeof(_desc) (desc) = (_desc); \
	cpu_to_le32s(&((desc)->word0)); \
	cpu_to_le32s(&((desc)->word1)); \
})

/* RX DESC size shift to obtain index from descriptor pointer. */
#define EDMAV2_RXDESC_SIZE_SHIFT		5

/**
 * struct edmav2_rxdesc_stats - RX descriptor ring stats.
 * @src_port_inval: Invalid source port number
 * @src_port_inval_type: Source type is not PORT ID
 * @src_port_inval_netdev: Invalid net device for the source port
 * @syncp: Synchronization pointer
 */
struct edmav2_rxdesc_stats {
	u64 src_port_inval;
	u64 src_port_inval_type;
	u64 src_port_inval_netdev;
	struct u64_stats_sync syncp;
};

/**
 * struct edmav2_rxfill_stats - Rx fill descriptor ring stats.
 * @alloc_failed: Buffer allocation failure count
 * @page_alloc_failed: Page allocation failure count for page mode
 * @syncp: Synchronization pointer
 */
struct edmav2_rxfill_stats {
	u64 alloc_failed;
	u64 page_alloc_failed;
	struct u64_stats_sync syncp;
};

/**
 * struct edmav2_rxdesc_pri - Rx descriptor.
 * @word0: Buffer address
 * @word1: More bit, priority bit, service code
 * @word2: Opaque low bits
 * @word3: Opaque high bits
 * @word4: Destination and source information
 * @word5: WiFi QoS, data length
 * @word6: Hash value, check sum status
 * @word7: DSCP, packet offsets
 */
struct edmav2_rxdesc_pri {
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
  * struct edmav2_rxdesc_sec - Rx secondary descriptor.
  * @word0: Timestamp
  * @word1: Secondary checksum status
  * @word2: QoS tag
  * @word3: Flow index details
  * @word4: Secondary packet offsets
  * @word5: Multicast bit, checksum
  * @word6: SVLAN, CVLAN
  * @word7: Secondary SVLAN, CVLAN
  */
struct edmav2_rxdesc_sec {
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
 * struct edmav2_rxfill_desc - RxFill descriptor.
 * @word0: Buffer address
 * @word1: Buffer size
 * @word2: Opaque low bits
 * @word3: Opaque high bits
 */
struct edmav2_rxfill_desc {
	u32 word0;
	u32 word1;
	u32 word2;
	u32 word3;
};

/**
 * struct edmav2_rxfill_ring - RxFill ring
 * @priv: EDMA that owns the ring
 * @ring_id: RxFill ring number
 * @count: Number of descriptors in the ring
 * @prod_idx: Ring producer index
 * @alloc_size: Buffer size to allocate
 * @desc: Descriptor ring virtual address
 * @dma: Descriptor ring physical address
 * @buf_len: Buffer length for rxfill descriptor
 * @rx_fill_stats: Rx fill ring statistics
 */
struct edmav2_rxfill_ring {
	struct edmav2 *priv;
	u32 ring_id;
	u32 count;
	u32 prod_idx;
	u32 alloc_size;
	struct edmav2_rxfill_desc *desc;
	dma_addr_t dma;
	u32 buf_len;
	struct edmav2_rxfill_stats rxfill_stats;
};

/**
 * struct edmav2_rxdesc_ring - RxDesc ring
 * @priv: EDMA that owns the ring
 * @napi: NAPI of the ring
 * @ring_id: Rxdesc ring number
 * @count: Number of descriptors in the ring
 * @work_leftover: Leftover descriptors to be processed
 * @cons_idx: Ring consumer index
 * @pdesc: Primary descriptor ring virtual address
 * @src_port: Source port of the frame that is being assembled
 * @data_offset: Data offset of the frame that is being assembled
 * @sdesc: Secondary descriptor ring virtual address
 * @rxdesc_stats: Rx descriptor ring statistics
 * @rxfill: RxFill ring used
 * @napi_added: Flag to indicate NAPI add status
 * @pdma: Primary descriptor ring physical address
 * @sdma: Secondary descriptor ring physical address
 * @head: Head of the skb list in case of scatter-gather frame
 * @last: Last skb of the skb list in case of scatter-gather frame
 */
struct edmav2_rxdesc_ring {
	struct edmav2 *priv;
	struct napi_struct napi;
	u32 ring_id;
	u32 count;
	u32 work_leftover;
	u32 cons_idx;
	struct edmav2_rxdesc_pri *pdesc;
	int src_port;
	u32 data_offset;
	struct edmav2_rxdesc_sec *sdesc;
	struct edmav2_rxdesc_stats rxdesc_stats;
	struct edmav2_rxfill_ring *rxfill;
	bool napi_added;
	dma_addr_t pdma;
	dma_addr_t sdma;
	struct sk_buff *head;
	struct sk_buff *last;
};

irqreturn_t edmav2_rx_handle_irq(int irq, void *ctx);
int edmav2_rx_alloc_buffer(struct edmav2_rxfill_ring *rxfill_ring, int alloc_count);
int edmav2_rx_napi_poll(struct napi_struct *napi, int budget);
#endif
