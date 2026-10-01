// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

/* The EDMA API. Each call is passed on to the implementation of the
 * hardware generation. The netdev table, the DSA tag and the resources that
 * every generation needs are handled here.
 */

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#include "edma.h"
#include "edmav2/edma.h"
#include "ppe.h"

#define EDMA_DSA_HLEN		4

/* The tag bytes after the two MAC addresses. The source or destination port
 * is in bits 7:3 of the second byte.
 */
#define EDMA_DSA_PORT_SHIFT	3
#define EDMA_DSA_PORT_MASK	0x1f

/* The commands FROM_CPU (1) and FORWARD (3) have bit 6 set. */
#define EDMA_DSA_CMD_FROM_CPU_BIT	BIT(6)

struct edma_clks {
	struct clk_bulk_data *clks;
	int num;
};

static void edma_clks_put(struct edma_clks *c, int num)
{
	int i;

	for (i = 0; i < num; i++)
		clk_put(c->clks[i].clk);
}

static void edma_clks_release(void *data)
{
	struct edma_clks *c = data;

	clk_bulk_disable_unprepare(c->num, c->clks);
	edma_clks_put(c, c->num);
}

static void edma_reset_release(void *data)
{
	reset_control_put(data);
}

/* The DSA core inserts the tag and the hardware wants the port in the
 * preheader, so the destination port is read here. A frame that is sent
 * directly on the conduit has no tag and no destination port. The command
 * field in the two top bits of the first tag byte tells the two cases apart,
 * because the DSA core only sends the commands FROM_CPU and FORWARD.
 */
static int edma_tx_tag_dst_port(struct sk_buff *skb, u8 *dst_port)
{
	u8 *tag;

	if (unlikely(!pskb_may_pull(skb, 2 * ETH_ALEN + EDMA_DSA_HLEN)))
		return -EINVAL;

	tag = skb->data + 2 * ETH_ALEN;
	if (unlikely(!(tag[0] & EDMA_DSA_CMD_FROM_CPU_BIT)))
		return -EINVAL;

	*dst_port = (tag[1] >> EDMA_DSA_PORT_SHIFT) & EDMA_DSA_PORT_MASK;

	return 0;
}

void edma_tx_tag_strip(struct sk_buff *skb)
{
	memmove(skb->data + EDMA_DSA_HLEN, skb->data, 2 * ETH_ALEN);
	skb_pull(skb, EDMA_DSA_HLEN);
}

/* The frame is rebuilt so that the DSA core finds the tag after the MAC
 * addresses. It is left with CHECKSUM_NONE, because the DSA receive path
 * pulls across bytes that no hardware checksum covered.
 */
static void edma_rx_tag_insert(struct sk_buff *skb, u8 src_port)
{
	u8 *tag;

	skb_push(skb, ETH_HLEN);
	skb_push(skb, EDMA_DSA_HLEN);
	memmove(skb->data, skb->data + EDMA_DSA_HLEN, 2 * ETH_ALEN);
	tag = skb->data + 2 * ETH_ALEN;
	tag[0] = 0;
	tag[1] = src_port << EDMA_DSA_PORT_SHIFT;
	tag[2] = 0;
	tag[3] = 0;
	skb_pull(skb, ETH_HLEN);
}

void edma_rx_deliver(struct edma *edma, struct napi_struct *napi,
		     struct sk_buff *skb, u8 src_port)
{
	struct net_device *netdev;
	unsigned int len = skb->len;

	if (unlikely(src_port >= EDMA_MAX_PORTS)) {
		dev_kfree_skb_any(skb);
		return;
	}

	netdev = edma_rx_netdev(edma, src_port);
	if (unlikely(!netdev)) {
		dev_kfree_skb_any(skb);
		return;
	}

	if (!(netdev->features & NETIF_F_RXHASH))
		skb_clear_hash(skb);

	/* The DSA core needs a protocol that eth_type_trans() derives from
	 * the untagged frame.
	 */
	skb->protocol = eth_type_trans(skb, netdev);
	if (edma->tag_mode == EDMA_TAG_DSA)
		edma_rx_tag_insert(skb, src_port);

	dev_sw_netstats_rx_add(netdev, len);
	napi_gro_receive(napi, skb);
}

int edma_irq_get(struct edma *edma, const char *fmt, ...)
{
	char name[32];
	va_list args;
	int irq;

	va_start(args, fmt);
	vsnprintf(name, sizeof(name), fmt, args);
	va_end(args);

	irq = of_irq_get_byname(edma->np, name);

	return irq ? irq : -ENXIO;
}

int edma_irq_request(struct edma *edma, int irq, irq_handler_t handler,
		     void *dev_id, const char *fmt, ...)
{
	va_list args;
	char *name;

	/* The name is printed by /proc/interrupts, so it lives as long as
	 * the device.
	 */
	va_start(args, fmt);
	name = devm_kvasprintf(edma->dev, GFP_KERNEL, fmt, args);
	va_end(args);
	if (!name)
		return -ENOMEM;

	return devm_request_irq(edma->dev, irq, handler, 0, name, dev_id);
}

int edma_register_netdev(struct edma *edma, u8 port_id,
			 struct net_device *netdev)
{
	if (port_id >= EDMA_MAX_PORTS)
		return -EINVAL;

	edma->netdev[port_id] = netdev;

	return 0;
}

void edma_unregister_netdev(struct edma *edma, u8 port_id)
{
	if (port_id < EDMA_MAX_PORTS)
		edma->netdev[port_id] = NULL;
}

/* The transmit queues of a netdev restart their byte accounting when the
 * processing starts. A netdev that serves several ports is reset once.
 */
static void edma_reset_tx_queues(struct edma *edma)
{
	int i, j;

	for (i = 0; i < EDMA_MAX_PORTS; i++) {
		struct net_device *netdev = edma->netdev[i];
		unsigned int q, n;

		if (!netdev)
			continue;

		for (j = 0; j < i; j++)
			if (edma->netdev[j] == netdev)
				break;
		if (j < i)
			continue;

		n = min(edma->caps.tx_queues, netdev->real_num_tx_queues);
		for (q = 0; q < n; q++)
			netdev_tx_reset_queue(netdev_get_tx_queue(netdev, q));
	}
}

int edma_open(struct edma *edma)
{
	int ret = 0;

	mutex_lock(&edma->lock);
	if (!edma->open_count) {
		edma_reset_tx_queues(edma);

		switch (edma->gen) {
		case EDMA_V2:
			edmav2_open(edma);
			break;
		default:
			ret = -ENODEV;
			break;
		}
	}
	if (!ret)
		edma->open_count++;
	mutex_unlock(&edma->lock);

	return ret;
}

void edma_close(struct edma *edma)
{
	mutex_lock(&edma->lock);
	if (WARN_ON(!edma->open_count))
		goto out;

	if (!--edma->open_count) {
		switch (edma->gen) {
		case EDMA_V2:
			edmav2_close(edma);
			break;
		default:
			break;
		}
	}
out:
	mutex_unlock(&edma->lock);
}

void edma_pause(struct edma *edma)
{
	mutex_lock(&edma->lock);
	if (edma->open_count) {
		switch (edma->gen) {
		case EDMA_V2:
			edmav2_close(edma);
			break;
		default:
			break;
		}
	}
	mutex_unlock(&edma->lock);
}

void edma_resume(struct edma *edma)
{
	mutex_lock(&edma->lock);
	if (edma->open_count) {
		switch (edma->gen) {
		case EDMA_V2:
			edmav2_open(edma);
			break;
		default:
			break;
		}
	}
	mutex_unlock(&edma->lock);
}

netdev_tx_t edma_xmit(struct edma *edma, struct sk_buff *skb, u8 dst_port,
		      u8 txq)
{
	if (edma->tag_mode == EDMA_TAG_DSA &&
	    edma_tx_tag_dst_port(skb, &dst_port))
		goto drop;

	switch (edma->gen) {
	case EDMA_V2:
		return edmav2_xmit(edma, skb, dst_port, txq);
	default:
		break;
	}

drop:
	DEV_STATS_INC(skb->dev, tx_dropped);
	dev_kfree_skb_any(skb);

	return NETDEV_TX_OK;
}

int edma_set_max_frame(struct edma *edma, unsigned int frame_size)
{
	switch (edma->gen) {
	case EDMA_V2:
		/* Frames that do not fit a buffer use several descriptors. */
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

const struct edma_stat_desc *edma_stats_layout(struct edma *edma,
					       unsigned int *count)
{
	switch (edma->gen) {
	case EDMA_V2:
		return edmav2_stats_layout(edma, count);
	default:
		*count = 0;
		return NULL;
	}
}

void edma_stats_read(struct edma *edma, u64 *buf)
{
	switch (edma->gen) {
	case EDMA_V2:
		edmav2_stats_read(edma, buf);
		break;
	default:
		break;
	}
}

int edma_ringparam_get(struct edma *edma, struct ethtool_ringparam *rp)
{
	switch (edma->gen) {
	case EDMA_V2:
		return edmav2_ringparam_get(edma, rp);
	default:
		return -EOPNOTSUPP;
	}
}

int edma_ringparam_set(struct edma *edma, struct ethtool_ringparam *rp)
{
	return -EOPNOTSUPP;
}

int edma_regs_len(struct edma *edma)
{
	switch (edma->gen) {
	default:
		return -EOPNOTSUPP;
	}
}

void edma_regs_dump(struct edma *edma, void *buf)
{
	switch (edma->gen) {
	default:
		break;
	}
}

/* Clocks and reset of the ethernet-dma node. The node has no driver of its
 * own, so they are managed with the device of the PPE.
 */
static int edma_get_resources(struct edma *edma)
{
	struct device *dev = edma->dev;
	struct edma_clks *c;
	struct reset_control *rst;
	int ret, i;

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;

	c->num = of_count_phandle_with_args(edma->np, "clocks", "#clock-cells");
	if (c->num < 0)
		return c->num;

	c->clks = devm_kcalloc(dev, c->num, sizeof(*c->clks), GFP_KERNEL);
	if (!c->clks)
		return -ENOMEM;

	for (i = 0; i < c->num; i++) {
		c->clks[i].clk = of_clk_get(edma->np, i);
		if (IS_ERR(c->clks[i].clk)) {
			ret = PTR_ERR(c->clks[i].clk);
			edma_clks_put(c, i);
			return ret;
		}

		/* The clocks of the EDMA v2 run at the rate of the PPE. */
		if (edma->gen == EDMA_V2) {
			ret = clk_set_rate(c->clks[i].clk, edma->ppe_dev->clk_rate);
			if (ret) {
				edma_clks_put(c, i + 1);
				return ret;
			}
		}
	}

	ret = clk_bulk_prepare_enable(c->num, c->clks);
	if (ret) {
		edma_clks_put(c, c->num);
		return ret;
	}

	ret = devm_add_action_or_reset(dev, edma_clks_release, c);
	if (ret)
		return ret;

	/* The EDMA v2 resets the resets that are in the node itself. */
	if (edma->gen == EDMA_V2)
		return 0;

	rst = of_reset_control_get_exclusive(edma->np, NULL);
	if (IS_ERR(rst))
		return PTR_ERR(rst);

	ret = devm_add_action_or_reset(dev, edma_reset_release, rst);
	if (ret)
		return ret;

	edma->rst = rst;

	return 0;
}

int edma_init(struct ppe_device *ppe_dev, const struct edma_config *cfg,
	      struct edma **edmap)
{
	struct device *dev = ppe_dev->dev;
	struct edma *edma;
	int ret;

	edma = devm_kzalloc(dev, sizeof(*edma), GFP_KERNEL);
	if (!edma)
		return -ENOMEM;

	edma->dev = dev;
	edma->ppe_dev = ppe_dev;
	edma->regmap = ppe_dev->regmap;
	edma->gen = ppe_dev->data->edma_gen;
	edma->soc_data = ppe_dev->data->edma_data;
	edma->tag_mode = cfg->tag_mode;
	mutex_init(&edma->lock);

	edma->np = of_get_child_by_name(dev->of_node, "ethernet-dma");
	if (!edma->np)
		return dev_err_probe(dev, -ENODEV, "no ethernet-dma node\n");

	ret = edma_get_resources(edma);
	if (ret)
		goto err_node;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		goto err_node;

	edma->dummy = alloc_netdev_dummy(0);
	if (!edma->dummy) {
		ret = -ENOMEM;
		goto err_node;
	}

	switch (edma->gen) {
	case EDMA_V2:
		ret = edmav2_init(edma);
		break;
	default:
		ret = -ENODEV;
		break;
	}
	if (ret)
		goto err_dummy;

	ret = dev_set_threaded(edma->dummy, NETDEV_NAPI_THREADED_ENABLED);
	if (ret)
		dev_warn(dev, "failed to enable threaded NAPI: %d\n", ret);

	*edmap = edma;

	return 0;

err_dummy:
	free_netdev(edma->dummy);
err_node:
	of_node_put(edma->np);
	return ret;
}

void edma_fini(struct edma *edma)
{
	switch (edma->gen) {
	case EDMA_V2:
		edmav2_fini(edma);
		break;
	default:
		break;
	}

	free_netdev(edma->dummy);
	of_node_put(edma->np);
}
