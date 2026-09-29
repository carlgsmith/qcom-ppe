// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

/* Qualcomm Ethernet DMA driver setup, HW configuration and interrupt
 * initializations for the IPQ9574 and IPQ5424 generation.
 */

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#include "../ppe.h"
#include "../ppe_config.h"
#include "cfg_rx.h"
#include "cfg_tx.h"
#include "edma.h"
#include "regs.h"

/* Priority to multi-queue mapping. */
static const u8 edmav2_pri_map[PPE_QUEUE_INTER_PRI_NUM] = {
	0, 1, 2, 3, 4, 5, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7};

/* Rx Fill ring info for IPQ9574. */
static const struct edmav2_ring_info ipq9574_rxfill_ring_info = {
	.max_rings = 8,
	.ring_start = 4,
	.num_rings = 4,
};

/* Rx ring info for IPQ9574. */
static const struct edmav2_ring_info ipq9574_rx_ring_info = {
	.max_rings = 24,
	.ring_start = 20,
	.num_rings = 4,
};

/* Tx ring info for IPQ9574. */
static const struct edmav2_ring_info ipq9574_tx_ring_info = {
	.max_rings = 32,
	.ring_start = 8,
	.num_rings = 24,
};

/* Tx complete ring info for IPQ9574. */
static const struct edmav2_ring_info ipq9574_txcmpl_ring_info = {
	.max_rings = 32,
	.ring_start = 8,
	.num_rings = 24,
};

/* HW info for IPQ9574. */
const struct edmav2_hw_info edmav2_ipq9574_data = {
	.rxfill = &ipq9574_rxfill_ring_info,
	.rx = &ipq9574_rx_ring_info,
	.tx = &ipq9574_tx_ring_info,
	.txcmpl = &ipq9574_txcmpl_ring_info,
	.max_ports = 6,
	.napi_budget_rx = 32,
	.napi_budget_tx = 512,
	.tso_max = 32,
	.idx_mask = 0xffff,
	.txdesc_fc_grp_id_mask = GENMASK(3, 1),
};

/* Rx Fill ring info for IPQ5424 */
static const struct edmav2_ring_info ipq5424_rxfill_ring_info = {
	.max_rings = 8,
	.ring_start = 4,
	.num_rings = 4,
};

/* Rx ring info for IPQ5424 */
static const struct edmav2_ring_info ipq5424_rx_ring_info = {
	.max_rings = 24,
	.ring_start = 20,
	.num_rings = 4,
};

/* Tx ring info for IPQ5424 */
static const struct edmav2_ring_info ipq5424_tx_ring_info = {
	.max_rings = 32,
	.ring_start = 4,
	.num_rings = 12,
};

/* Tx complete ring info for IPQ5424 */
static const struct edmav2_ring_info ipq5424_txcmpl_ring_info = {
	.max_rings = 32,
	.ring_start = 4,
	.num_rings = 12,
};

/* HW info for IPQ5424 */
const struct edmav2_hw_info edmav2_ipq5424_data = {
	.rxfill = &ipq5424_rxfill_ring_info,
	.rx = &ipq5424_rx_ring_info,
	.tx = &ipq5424_tx_ring_info,
	.txcmpl = &ipq5424_txcmpl_ring_info,
	.max_ports = 3,
	.napi_budget_rx = 128,
	.napi_budget_tx = 256,
	.tso_max = 48,
	.idx_mask = 0xffffffff,
	.txdesc_fc_grp_id_mask = GENMASK(4, 1),
	.rxdesc_no_pl_offset = true,
	.rxfill_size_in_buffer1_reg = true,
};

/**
 * edmav2_configure_ucast_prio_map_tbl - Configure unicast priority map table.
 * @priv: EDMA
 *
 * Map int_priority values to priority class and initialize
 * unicast priority map table for default profile_id.
 */
static int edmav2_configure_ucast_prio_map_tbl(struct edmav2 *priv)
{
	u8 pri_class, int_pri;
	int ret;

	/* Set the priority class value for every possible priority. */
	for (int_pri = 0; int_pri < PPE_QUEUE_INTER_PRI_NUM; int_pri++) {
		pri_class = edmav2_pri_map[int_pri];

		/* Priority offset should be less than maximum supported
		 * queue priority.
		 */
		if (pri_class > EDMAV2_PRI_MAX_PER_CORE - 1)
			return -EINVAL;

		ret = ppe_queue_ucast_offset_pri_set(priv->ppe_dev,
						     EDMAV2_CPU_PORT_PROFILE_ID,
						     int_pri, pri_class);
		if (ret)
			return ret;
	}

	return 0;
}

static void edmav2_disable_misc_interrupt(struct edmav2 *priv)
{
	regmap_write(priv->ppe_dev->regmap,
		     EDMA_BASE_OFFSET + EDMAV2_REG_MISC_INT_MASK_ADDR,
		     EDMAV2_MASK_INT_CLEAR);
}

static void edmav2_enable_misc_interrupt(struct edmav2 *priv)
{
	regmap_write(priv->ppe_dev->regmap,
		     EDMA_BASE_OFFSET + EDMAV2_REG_MISC_INT_MASK_ADDR,
		     priv->intr_info.intr_mask_misc);
}

/* The miscellaneous errors, each with the counter that counts it. */
static const struct {
	u32 mask;
	size_t offset;
	const char *msg;
} edmav2_misc_errors[] = {
	{ EDMAV2_MISC_AXI_RD_ERR_MASK,
	  offsetof(struct edmav2_err_stats, edmav2_axi_read_err),
	  "AXI read error" },
	{ EDMAV2_MISC_AXI_WR_ERR_MASK,
	  offsetof(struct edmav2_err_stats, edmav2_axi_write_err),
	  "AXI write error" },
	{ EDMAV2_MISC_RX_DESC_FIFO_FULL_MASK,
	  offsetof(struct edmav2_err_stats, edmav2_rxdesc_fifo_full),
	  "Rx descriptor fifo full error" },
	{ EDMAV2_MISC_RX_ERR_BUF_SIZE_MASK,
	  offsetof(struct edmav2_err_stats, edmav2_rx_buf_size_err),
	  "Rx buffer size error" },
	{ EDMAV2_MISC_TX_SRAM_FULL_MASK,
	  offsetof(struct edmav2_err_stats, edmav2_tx_sram_full),
	  "Tx SRAM full error" },
	{ EDMAV2_MISC_TX_CMPL_BUF_FULL_MASK,
	  offsetof(struct edmav2_err_stats, edmav2_txcmpl_buf_full),
	  "Tx complete buffer full error" },
	{ EDMAV2_MISC_DATA_LEN_ERR_MASK,
	  offsetof(struct edmav2_err_stats, edmav2_tx_data_len_err),
	  "data length error" },
	{ EDMAV2_MISC_TX_TIMEOUT_MASK,
	  offsetof(struct edmav2_err_stats, edmav2_tx_timeout),
	  "Tx timeout error" },
};

static irqreturn_t edmav2_misc_handle_irq(int irq, void *ctx)
{
	struct edmav2 *priv = ctx;
	struct edmav2_err_stats *stats = this_cpu_ptr(priv->err_stats);
	struct regmap *regmap = priv->ppe_dev->regmap;
	u32 misc_intr_status, data;
	int i;

	/* Read Misc intr status */
	regmap_read(regmap, EDMA_BASE_OFFSET + EDMAV2_REG_MISC_INT_STAT_ADDR, &data);
	misc_intr_status = data & priv->intr_info.intr_mask_misc;

	for (i = 0; i < ARRAY_SIZE(edmav2_misc_errors); i++) {
		if (!(misc_intr_status & edmav2_misc_errors[i].mask))
			continue;

		dev_err_ratelimited(priv->ppe_dev->dev, "MISC %s received\n",
				    edmav2_misc_errors[i].msg);
		u64_stats_update_begin(&stats->syncp);
		(*(u64 *)((void *)stats + edmav2_misc_errors[i].offset))++;
		u64_stats_update_end(&stats->syncp);
	}

	return IRQ_HANDLED;
}

static int edmav2_irq_register(struct edmav2 *priv)
{
	const struct edmav2_hw_info *hw_info = priv->hw_info;
	const struct edmav2_ring_info *txcmpl = hw_info->txcmpl;
	const struct edmav2_ring_info *rx = hw_info->rx;
	struct device *dev = priv->ppe_dev->dev;
	u32 i;
	int ret;

	for (i = 0; i < txcmpl->num_rings; i++) {
		irq_set_status_flags(priv->intr_info.intr_txcmpl[i], IRQ_DISABLE_UNLAZY);

		ret = edma_irq_request(priv->edma, priv->intr_info.intr_txcmpl[i],
				       edmav2_tx_handle_irq, &priv->txcmpl_rings[i],
				       "edma_txcmpl_%u", txcmpl->ring_start + i);
		if (ret)
			return dev_err_probe(dev, ret, "TXCMPL ring IRQ:%d request %d failed\n",
					     priv->intr_info.intr_txcmpl[i], i);
	}

	for (i = 0; i < rx->num_rings; i++) {
		irq_set_status_flags(priv->intr_info.intr_rx[i], IRQ_DISABLE_UNLAZY);

		ret = edma_irq_request(priv->edma, priv->intr_info.intr_rx[i],
				       edmav2_rx_handle_irq, &priv->rx_rings[i],
				       "edma_rxdesc_%u", rx->ring_start + i);
		if (ret)
			return dev_err_probe(dev, ret, "RXDESC ring IRQ:%d request failed\n",
					     priv->intr_info.intr_rx[i]);
	}

	ret = edma_irq_request(priv->edma, priv->intr_info.intr_misc,
			       edmav2_misc_handle_irq, priv, "edma_misc");
	if (ret)
		return dev_err_probe(dev, ret, "MISC IRQ:%d request failed\n",
				     priv->intr_info.intr_misc);

	return 0;
}

/* Get the interrupt numbers of the rings by their names in the node. */
static int edmav2_irq_configure(struct edmav2 *priv)
{
	const struct edmav2_hw_info *hw_info = priv->hw_info;
	const struct edmav2_ring_info *txcmpl = hw_info->txcmpl;
	const struct edmav2_ring_info *rx = hw_info->rx;
	struct device *dev = priv->ppe_dev->dev;
	u32 i;

	priv->intr_info.intr_txcmpl = devm_kcalloc(dev, txcmpl->num_rings,
						   sizeof(*priv->intr_info.intr_txcmpl),
						   GFP_KERNEL);
	priv->intr_info.intr_rx = devm_kcalloc(dev, rx->num_rings,
					       sizeof(*priv->intr_info.intr_rx),
					       GFP_KERNEL);
	if (!priv->intr_info.intr_txcmpl || !priv->intr_info.intr_rx)
		return -ENOMEM;

	/* Get TXCMPL rings IRQ numbers. */
	for (i = 0; i < txcmpl->num_rings; i++) {
		priv->intr_info.intr_txcmpl[i] = edma_irq_get(priv->edma, "txcmpl_%u",
							      txcmpl->ring_start + i);
		if (priv->intr_info.intr_txcmpl[i] < 0)
			return dev_err_probe(dev, priv->intr_info.intr_txcmpl[i],
					     "txcmpl_%u: irq get failed\n",
					     txcmpl->ring_start + i);
	}

	/* Get RXDESC rings IRQ numbers. */
	for (i = 0; i < rx->num_rings; i++) {
		priv->intr_info.intr_rx[i] = edma_irq_get(priv->edma, "rxdesc_%u",
							  rx->ring_start + i);
		if (priv->intr_info.intr_rx[i] < 0)
			return dev_err_probe(dev, priv->intr_info.intr_rx[i],
					     "rxdesc_%u: irq get failed\n",
					     rx->ring_start + i);
	}

	/* Get misc IRQ number. */
	priv->intr_info.intr_misc = edma_irq_get(priv->edma, "misc");
	if (priv->intr_info.intr_misc < 0)
		return dev_err_probe(dev, priv->intr_info.intr_misc,
				     "misc irq get failed\n");

	return 0;
}

static int edmav2_alloc_rings(struct edmav2 *priv)
{
	int ret;

	ret = edmav2_cfg_tx_rings_alloc(priv);
	if (ret)
		return ret;

	ret = edmav2_cfg_rx_rings_alloc(priv);
	if (ret)
		edmav2_cfg_tx_rings_cleanup(priv);

	return ret;
}

/* The EDMA node has one reset or several, which are all asserted and
 * released together.
 */
static int edmav2_hw_reset(struct edmav2 *priv)
{
	struct reset_control *rst;

	rst = of_reset_control_array_get_exclusive(priv->edma->np);
	if (IS_ERR(rst))
		return PTR_ERR(rst);

	/* 100us delay is required by hardware to reset EDMA. */
	reset_control_assert(rst);
	fsleep(100);

	reset_control_deassert(rst);
	fsleep(100);

	reset_control_put(rst);

	return 0;
}

/* The port control register has the global enable. */
static void edmav2_stop_port(struct edmav2 *priv)
{
	regmap_write(priv->ppe_dev->regmap,
		     EDMA_BASE_OFFSET + EDMAV2_REG_PORT_CTRL_ADDR, 0);
}

/* Stop the EDMA and all of its rings before it is reset and configured. */
static void edmav2_disable_all_rings(struct edmav2 *priv)
{
	const struct edmav2_hw_info *hw_info = priv->hw_info;
	struct regmap *regmap = priv->ppe_dev->regmap;
	u32 i;

	edmav2_stop_port(priv);

	edmav2_cfg_rx_rings_disable_all(priv);

	for (i = 0; i < hw_info->tx->max_rings; i++) {
		u32 reg = EDMA_BASE_OFFSET + EDMAV2_REG_TXDESC_CTRL(i);
		u32 data;

		regmap_clear_bits(regmap, reg, EDMAV2_TXDESC_CTRL_TXEN_MASK);
		if (regmap_read_poll_timeout(regmap, reg, data,
					     !(data & EDMAV2_TXDESC_CTRL_TXEN_MASK),
					     10, 10000))
			dev_warn(priv->ppe_dev->dev, "Tx ring %u did not stop\n", i);
	}

	/* Let the transfers in flight finish. */
	fsleep(100);
}

static int edmav2_hw_configure(struct edmav2 *priv)
{
	struct regmap *regmap = priv->ppe_dev->regmap;
	struct device *dev = priv->ppe_dev->dev;
	u32 data, reg;
	int ret;

	priv->intr_info.intr_mask_rx = EDMAV2_RXDESC_INT_MASK_PKT_INT;
	priv->intr_info.intr_mask_txcmpl = EDMAV2_TX_INT_MASK_PKT_INT;

	edmav2_disable_all_rings(priv);

	ret = edmav2_hw_reset(priv);
	if (ret)
		return dev_err_probe(dev, ret, "Error in resetting the hardware\n");

	ret = edmav2_alloc_rings(priv);
	if (ret)
		return dev_err_probe(dev, ret, "Error in initializing the rings\n");

	/* Disable interrupts. */
	edmav2_cfg_tx_disable_interrupts(priv);
	edmav2_cfg_rx_disable_interrupts(priv);
	edmav2_disable_misc_interrupt(priv);

	edmav2_cfg_rx_rings_disable(priv);

	edmav2_cfg_rx_ring_mappings(priv);
	edmav2_cfg_tx_ring_mappings(priv);

	edmav2_cfg_tx_rings(priv);

	ret = edmav2_cfg_rx_rings(priv);
	if (ret) {
		dev_err(dev, "Error in configuring Rx rings, ret: %d\n", ret);
		goto err_rings;
	}

	/* Configure DMA request priority, DMA read burst length,
	 * and AXI write size.
	 */
	data = FIELD_PREP(EDMAV2_DMAR_BURST_LEN_MASK, EDMAV2_BURST_LEN_ENABLE);
	data |= FIELD_PREP(EDMAV2_DMAR_REQ_PRI_MASK, 0);
	data |= FIELD_PREP(EDMAV2_DMAR_TXDATA_OUTSTANDING_NUM_MASK, 31);
	data |= FIELD_PREP(EDMAV2_DMAR_TXDESC_OUTSTANDING_NUM_MASK, 7);
	data |= FIELD_PREP(EDMAV2_DMAR_RXFILL_OUTSTANDING_NUM_MASK, 7);

	reg = EDMA_BASE_OFFSET + EDMAV2_REG_DMAR_CTRL_ADDR;
	ret = regmap_write(regmap, reg, data);
	if (ret)
		goto err_rings;

	reg = EDMA_BASE_OFFSET + EDMAV2_REG_TX_TIMEOUT_THRESH_ADDR;
	ret = regmap_write(regmap, reg, EDMAV2_TX_TIMEOUT_THRESH_VAL);
	if (ret)
		goto err_rings;

	/* Set Miscellaneous error interrupt mask. */
	priv->intr_info.intr_mask_misc = EDMAV2_MISC_AXI_RD_ERR_MASK |
		EDMAV2_MISC_AXI_WR_ERR_MASK |
		EDMAV2_MISC_RX_DESC_FIFO_FULL_MASK |
		EDMAV2_MISC_RX_ERR_BUF_SIZE_MASK |
		EDMAV2_MISC_TX_SRAM_FULL_MASK |
		EDMAV2_MISC_TX_CMPL_BUF_FULL_MASK |
		EDMAV2_MISC_DATA_LEN_ERR_MASK |
		EDMAV2_MISC_TX_TIMEOUT_MASK;

	edmav2_cfg_rx_rings_enable(priv);
	edmav2_cfg_rx_napi_add(priv);
	edmav2_cfg_tx_napi_add(priv);

	/* Enable whole edma to work and padding if packet length less than 60
	 * byte in EDMA port interface control register.
	 */
	data = EDMAV2_PORT_PAD_EN | EDMAV2_PORT_EDMA_EN;
	reg = EDMA_BASE_OFFSET + EDMAV2_REG_PORT_CTRL_ADDR;
	ret = regmap_write(regmap, reg, data);
	if (ret)
		goto err_napi;

	ret = edmav2_configure_ucast_prio_map_tbl(priv);
	if (ret) {
		dev_err(dev, "Failed to initialize unicast priority map table: %d\n",
			ret);
		goto err_napi;
	}

	ret = edmav2_cfg_rx_rps_hash_map(priv);
	if (ret) {
		dev_err(dev, "Failed to configure rps hash table: %d\n", ret);
		goto err_napi;
	}

	return 0;

err_napi:
	edmav2_cfg_tx_napi_delete(priv);
	edmav2_cfg_rx_napi_delete(priv);
	edmav2_cfg_rx_rings_disable(priv);
err_rings:
	edmav2_cfg_tx_rings_cleanup(priv);
	edmav2_cfg_rx_rings_cleanup(priv);

	return ret;
}

/**
 * edmav2_fini - EDMA Destroy.
 * @edma: EDMA
 *
 * Free the memory allocated during setup.
 */
void edmav2_fini(struct edma *edma)
{
	struct edmav2 *priv = edma->priv;
	const struct edmav2_hw_info *hw_info = priv->hw_info;
	struct device *dev = priv->ppe_dev->dev;
	u32 i;

	edmav2_cfg_tx_disable_interrupts(priv);
	edmav2_cfg_rx_disable_interrupts(priv);
	edmav2_disable_misc_interrupt(priv);

	/* The handlers use the rings, so they end before the rings are freed. */
	for (i = 0; i < hw_info->txcmpl->num_rings; i++)
		devm_free_irq(dev, priv->intr_info.intr_txcmpl[i],
			      &priv->txcmpl_rings[i]);

	for (i = 0; i < hw_info->rx->num_rings; i++)
		devm_free_irq(dev, priv->intr_info.intr_rx[i], &priv->rx_rings[i]);

	devm_free_irq(dev, priv->intr_info.intr_misc, priv);

	edmav2_cfg_tx_napi_delete(priv);
	edmav2_cfg_rx_napi_delete(priv);
	edmav2_cfg_rx_rings_disable(priv);
	edmav2_stop_port(priv);
	edmav2_cfg_rx_rings_cleanup(priv);
	edmav2_cfg_tx_rings_cleanup(priv);

	free_percpu(priv->err_stats);
}

/**
 * edmav2_open - Start the receive and transmit processing.
 * @edma: EDMA
 */
void edmav2_open(struct edma *edma)
{
	struct edmav2 *priv = edma->priv;

	edmav2_cfg_rx_napi_enable(priv);
	edmav2_cfg_tx_napi_enable(priv);
	edmav2_cfg_tx_rings_enable(priv);
	edmav2_cfg_rx_enable_interrupts(priv);
	edmav2_cfg_tx_enable_interrupts(priv);
}

/**
 * edmav2_close - Stop the receive and transmit processing.
 * @edma: EDMA
 */
void edmav2_close(struct edma *edma)
{
	struct edmav2 *priv = edma->priv;

	edmav2_cfg_tx_disable_interrupts(priv);
	edmav2_cfg_rx_disable_interrupts(priv);
	edmav2_cfg_tx_napi_disable(priv);
	edmav2_cfg_rx_napi_disable(priv);
	edmav2_cfg_tx_rings_disable(priv);
}

/**
 * edmav2_init - EDMA Setup.
 * @edma: EDMA
 *
 * Configure EDMA hardware and interrupts. The clocks are enabled by the
 * caller.
 *
 * Return 0 on success, negative error code on failure.
 */
int edmav2_init(struct edma *edma)
{
	struct device *dev = edma->dev;
	struct edmav2 *priv;
	int ret, i;

	if (!edma->soc_data)
		return -EINVAL;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->edma = edma;
	priv->ppe_dev = edma->ppe_dev;
	priv->hw_info = edma->soc_data;

	priv->err_stats = alloc_percpu(*priv->err_stats);
	if (!priv->err_stats)
		return -ENOMEM;

	for_each_possible_cpu(i)
		u64_stats_init(&per_cpu_ptr(priv->err_stats, i)->syncp);

	edma->priv = priv;

	ret = edmav2_irq_configure(priv);
	if (ret)
		goto err_stats;

	ret = edmav2_hw_configure(priv);
	if (ret)
		goto err_stats;

	ret = edmav2_irq_register(priv);
	if (ret)
		goto err_hw;

	edma->caps = (struct edma_caps) {
		.rx_queues = priv->hw_info->rx->num_rings,
		.tx_queues = num_possible_cpus(),
		.max_tx_segs = priv->hw_info->tso_max,
		.max_mtu = ETH_MAX_MTU,
		.features = NETIF_F_FRAGLIST | NETIF_F_SG | NETIF_F_RXCSUM |
			    NETIF_F_HW_CSUM | NETIF_F_TSO | NETIF_F_TSO6,
	};

	edmav2_enable_misc_interrupt(priv);

	dev_info(dev, "EDMA v2: %u RX and %u TX queues\n", edma->caps.rx_queues,
		 edma->caps.tx_queues);

	return 0;

err_hw:
	edmav2_cfg_tx_napi_delete(priv);
	edmav2_cfg_rx_napi_delete(priv);
	edmav2_cfg_rx_rings_disable(priv);
	edmav2_cfg_tx_rings_cleanup(priv);
	edmav2_cfg_rx_rings_cleanup(priv);
err_stats:
	free_percpu(priv->err_stats);

	return ret;
}

static const struct edma_stat_desc edmav2_stat_descs[] = {
	{ .name = "axi_read_err", .group = EDMA_STAT_ERR },
	{ .name = "axi_write_err", .group = EDMA_STAT_ERR },
	{ .name = "rxdesc_fifo_full", .group = EDMA_STAT_ERR },
	{ .name = "rx_buf_size_err", .group = EDMA_STAT_ERR },
	{ .name = "tx_sram_full", .group = EDMA_STAT_ERR },
	{ .name = "tx_data_len_err", .group = EDMA_STAT_ERR },
	{ .name = "tx_timeout", .group = EDMA_STAT_ERR },
	{ .name = "txcmpl_buf_full", .group = EDMA_STAT_ERR },
	{ .name = "rx_alloc_failed", .group = EDMA_STAT_RX_RING },
	{ .name = "rx_src_port_inval", .group = EDMA_STAT_RX_RING },
	{ .name = "rx_src_port_inval_type", .group = EDMA_STAT_RX_RING },
	{ .name = "rx_src_port_inval_netdev", .group = EDMA_STAT_RX_RING },
	{ .name = "tx_no_desc_avail", .group = EDMA_STAT_TX_RING },
	{ .name = "tx_tso_max_seg_exceed", .group = EDMA_STAT_TX_RING },
	{ .name = "txcmpl_invalid_buffer", .group = EDMA_STAT_TX_RING },
	{ .name = "txcmpl_errors", .group = EDMA_STAT_TX_RING },
	{ .name = "txcmpl_desc_with_more_bit", .group = EDMA_STAT_TX_RING },
	{ .name = "txcmpl_no_pending_desc", .group = EDMA_STAT_TX_RING },
};

const struct edma_stat_desc *edmav2_stats_layout(struct edma *edma,
						 unsigned int *count)
{
	*count = ARRAY_SIZE(edmav2_stat_descs);

	return edmav2_stat_descs;
}

/* The counters of the rings are added up. The order is that of the layout. */
void edmav2_stats_read(struct edma *edma, u64 *buf)
{
	struct edmav2 *priv = edma->priv;
	const struct edmav2_hw_info *hw_info = priv->hw_info;
	u64 *out = buf;
	u64 sum;
	int cpu;
	u32 i;

	for (i = 0; i < ARRAY_SIZE(edmav2_misc_errors); i++) {
		sum = 0;
		for_each_possible_cpu(cpu) {
			struct edmav2_err_stats *stats = per_cpu_ptr(priv->err_stats, cpu);

			sum += *(u64 *)((void *)stats + edmav2_misc_errors[i].offset);
		}
		*out++ = sum;
	}

	sum = 0;
	for (i = 0; i < hw_info->rxfill->num_rings; i++)
		sum += priv->rxfill_rings[i].rxfill_stats.alloc_failed;
	*out++ = sum;

	*out = 0;
	for (i = 0; i < hw_info->rx->num_rings; i++)
		*out += priv->rx_rings[i].rxdesc_stats.src_port_inval;
	out++;

	*out = 0;
	for (i = 0; i < hw_info->rx->num_rings; i++)
		*out += priv->rx_rings[i].rxdesc_stats.src_port_inval_type;
	out++;

	*out = 0;
	for (i = 0; i < hw_info->rx->num_rings; i++)
		*out += priv->rx_rings[i].rxdesc_stats.src_port_inval_netdev;
	out++;

	*out = 0;
	for (i = 0; i < hw_info->tx->num_rings; i++)
		*out += priv->tx_rings[i].txdesc_stats.no_desc_avail;
	out++;

	*out = 0;
	for (i = 0; i < hw_info->tx->num_rings; i++)
		*out += priv->tx_rings[i].txdesc_stats.tso_max_seg_exceed;
	out++;

	*out = 0;
	for (i = 0; i < hw_info->txcmpl->num_rings; i++)
		*out += priv->txcmpl_rings[i].txcmpl_stats.invalid_buffer;
	out++;

	*out = 0;
	for (i = 0; i < hw_info->txcmpl->num_rings; i++)
		*out += priv->txcmpl_rings[i].txcmpl_stats.errors;
	out++;

	*out = 0;
	for (i = 0; i < hw_info->txcmpl->num_rings; i++)
		*out += priv->txcmpl_rings[i].txcmpl_stats.desc_with_more_bit;
	out++;

	*out = 0;
	for (i = 0; i < hw_info->txcmpl->num_rings; i++)
		*out += priv->txcmpl_rings[i].txcmpl_stats.no_pending_desc;
}

int edmav2_ringparam_get(struct edma *edma, struct ethtool_ringparam *rp)
{
	rp->tx_max_pending = EDMAV2_TX_RING_SIZE;
	rp->rx_max_pending = EDMAV2_RX_RING_SIZE;
	rp->tx_pending = EDMAV2_TX_RING_SIZE;
	rp->rx_pending = EDMAV2_RX_RING_SIZE;

	return 0;
}
