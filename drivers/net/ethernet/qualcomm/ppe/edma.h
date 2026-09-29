/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

/* The Ethernet DMA (EDMA) API that the port models use. The EDMA moves
 * frames between memory and the PPE and creates no netdev of its own.
 */

#ifndef __EDMA_H__
#define __EDMA_H__

#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/ethtool.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/regmap.h>
#include <linux/skbuff.h>

struct device_node;
struct ppe_device;
struct regmap;
struct reset_control;

/* The EDMA registers are part of the PPE register map. */
#define EDMA_BASE_OFFSET	0xb00000

#define EDMA_MAX_PORTS		8

/**
 * enum edma_gen - EDMA hardware generation.
 * @EDMA_NONE: The SoC has no EDMA support.
 * @EDMA_V1: 2-word descriptors with a software preheader.
 * @EDMA_V2: 8-word descriptors.
 */
enum edma_gen {
	EDMA_NONE,
	EDMA_V1,
	EDMA_V2,
};

/**
 * enum edma_tag_mode - Frame format on the conduit.
 * @EDMA_TAG_NONE: Frames carry no tag.
 * @EDMA_TAG_DSA: Received frames get the DSA tag of their source port and
 *	the destination port of a transmitted frame is read from its DSA
 *	tag. The tag format is the 4-byte tag of DSA_TAG_PROTO_DSA.
 *
 * The mode must agree with the tag protocol that the DSA driver reports.
 */
enum edma_tag_mode {
	EDMA_TAG_NONE,
	EDMA_TAG_DSA,
};

/**
 * struct edma_config - EDMA configuration given by the port model.
 * @tag_mode: Frame format on the conduit.
 */
struct edma_config {
	enum edma_tag_mode tag_mode;
};

/**
 * struct edma_caps - What the port model needs to know to set up a netdev.
 * @rx_queues: Number of RX queues.
 * @tx_queues: Number of TX queues.
 * @needed_headroom: Headroom the datapath needs in front of a frame.
 * @min_tx_frame: Minimum frame length, 0 if there is none.
 * @max_tx_segs: Largest number of buffers of one frame.
 * @max_mtu: Largest MTU.
 * @features: Offload features the datapath supports.
 */
struct edma_caps {
	unsigned int rx_queues;
	unsigned int tx_queues;
	unsigned int needed_headroom;
	unsigned int min_tx_frame;
	unsigned int max_tx_segs;
	unsigned int max_mtu;
	netdev_features_t features;
};

/**
 * enum edma_stat_group - Statistics groups.
 * @EDMA_STAT_GLOBAL: Counters of the whole engine.
 * @EDMA_STAT_PORT: Counters of one port.
 * @EDMA_STAT_RX_RING: Counters of one RX ring.
 * @EDMA_STAT_TX_RING: Counters of one TX ring.
 * @EDMA_STAT_ERR: Error counters.
 */
enum edma_stat_group {
	EDMA_STAT_GLOBAL,
	EDMA_STAT_PORT,
	EDMA_STAT_RX_RING,
	EDMA_STAT_TX_RING,
	EDMA_STAT_ERR,
};

/**
 * struct edma_stat_desc - Description of one counter.
 * @name: Counter name.
 * @group: Group of the counter.
 * @index: Ring or port number, when the group has several.
 */
struct edma_stat_desc {
	const char *name;
	enum edma_stat_group group;
	u16 index;
};

/**
 * struct edma - EDMA instance.
 * @dev: Device that owns the EDMA, the PPE device.
 * @ppe_dev: PPE that the EDMA is part of.
 * @np: The ethernet-dma node.
 * @regmap: PPE register map.
 * @rst: EDMA reset.
 * @gen: Hardware generation.
 * @tag_mode: Frame format on the conduit.
 * @soc_data: Data of the SoC for the implementation.
 * @caps: Capabilities, filled by the implementation.
 * @netdev: Netdev that receives the frames of each source port.
 * @dummy: Netdev that hosts the NAPI instances.
 * @lock: Protects @open_count.
 * @open_count: Number of users that opened the EDMA.
 * @priv: Data of the implementation.
 */
struct edma {
	struct device *dev;
	struct ppe_device *ppe_dev;
	struct device_node *np;
	struct regmap *regmap;
	struct reset_control *rst;
	enum edma_gen gen;
	enum edma_tag_mode tag_mode;
	const void *soc_data;
	struct edma_caps caps;
	struct net_device *netdev[EDMA_MAX_PORTS];
	struct net_device *dummy;
	/* Protects open_count. */
	struct mutex lock;
	unsigned int open_count;
	void *priv;
};

struct edmav2_hw_info;
extern const struct edmav2_hw_info edmav2_ipq9574_data;
extern const struct edmav2_hw_info edmav2_ipq5424_data;

/* Lifecycle */
int edma_init(struct ppe_device *ppe_dev, const struct edma_config *cfg,
	      struct edma **edmap);
void edma_fini(struct edma *edma);

/* The netdev that receives the frames of a port. The netdev must count its
 * traffic with the per-CPU tstats.
 */
int edma_register_netdev(struct edma *edma, u8 port_id,
			 struct net_device *netdev);
void edma_unregister_netdev(struct edma *edma, u8 port_id);

/* The first open starts the receive and transmit processing and the last
 * close stops it. Stop the TX queues of the netdevs before edma_close().
 */
int edma_open(struct edma *edma);
void edma_close(struct edma *edma);

/* Datapath. edma_xmit() takes a frame of the netdev in skb->dev. In the DSA
 * tag mode it reads the destination port from the tag and ignores @dst_port.
 *
 * edma_set_max_frame() sizes the receive buffers for a frame of at most
 * @frame_size bytes, counted from the MAC header to the end of the payload
 * with room for two VLAN tags. It needs the TX queues stopped and the
 * processing paused with edma_pause(), and the processing restarted with
 * edma_resume() afterwards.
 */
netdev_tx_t edma_xmit(struct edma *edma, struct sk_buff *skb, u8 dst_port,
		      u8 txq);
int edma_set_max_frame(struct edma *edma, unsigned int frame_size);
void edma_pause(struct edma *edma);
void edma_resume(struct edma *edma);

/* The netdev that receives the frames of a port. */
static inline struct net_device *edma_rx_netdev(struct edma *edma, u8 src_port)
{
	/* The conduit is registered as port 0. */
	return READ_ONCE(edma->netdev[edma->tag_mode == EDMA_TAG_DSA ? 0 : src_port]);
}

/* Information */
static inline const struct edma_caps *edma_caps_get(struct edma *edma)
{
	return &edma->caps;
}

const struct edma_stat_desc *edma_stats_layout(struct edma *edma,
					       unsigned int *count);
void edma_stats_read(struct edma *edma, u64 *buf);

/* Optional. Return -EOPNOTSUPP when the generation has no support. */
int edma_ringparam_get(struct edma *edma, struct ethtool_ringparam *rp);
int edma_ringparam_set(struct edma *edma, struct ethtool_ringparam *rp);
int edma_regs_len(struct edma *edma);
void edma_regs_dump(struct edma *edma, void *buf);

/* For the implementations. The interrupts of the rings are named in the
 * ethernet-dma node. edma_irq_get() finds one by name, and edma_irq_request()
 * requests one under a name that stays valid for the life of the device.
 * The tools that set the affinity of the interrupts look for the names
 * "edma_rxdesc" and "edma_txcmpl" in /proc/interrupts.
 */
__printf(2, 3)
int edma_irq_get(struct edma *edma, const char *fmt, ...);
__printf(5, 6)
int edma_irq_request(struct edma *edma, int irq, irq_handler_t handler,
		     void *dev_id, const char *fmt, ...);

/* edma_rx_deliver() takes a finished frame with the
 * hash and the checksum status already set. An implementation calls
 * edma_tx_tag_strip() only once it has accepted the frame, because a frame
 * that it refuses is requeued as it was handed over.
 */
void edma_rx_deliver(struct edma *edma, struct napi_struct *napi,
		     struct sk_buff *skb, u8 src_port);
void edma_tx_tag_strip(struct sk_buff *skb);

static inline int edma_read(struct edma *edma, u32 reg, u32 *val)
{
	return regmap_read(edma->regmap, EDMA_BASE_OFFSET + reg, val);
}

static inline int edma_write(struct edma *edma, u32 reg, u32 val)
{
	return regmap_write(edma->regmap, EDMA_BASE_OFFSET + reg, val);
}

static inline int edma_set_bits(struct edma *edma, u32 reg, u32 bits)
{
	return regmap_set_bits(edma->regmap, EDMA_BASE_OFFSET + reg, bits);
}

static inline int edma_clear_bits(struct edma *edma, u32 reg, u32 bits)
{
	return regmap_clear_bits(edma->regmap, EDMA_BASE_OFFSET + reg, bits);
}

static inline int edma_update_bits(struct edma *edma, u32 reg, u32 mask,
				   u32 val)
{
	return regmap_update_bits(edma->regmap, EDMA_BASE_OFFSET + reg, mask,
				  val);
}

#endif /* __EDMA_H__ */
