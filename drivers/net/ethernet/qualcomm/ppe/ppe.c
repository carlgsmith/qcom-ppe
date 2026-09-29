// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

/* PPE platform device probe, DTSI parser and PPE clock initializations. */

#include <linux/clk.h>
#include <linux/interconnect.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#include "ppe.h"
#include "ppe_config.h"
#include "ppe_debugfs.h"
#include "ppe_mac.h"
#include "ppe_port.h"

#define PPE_PORT_MAX		8
#define PPE_CLK_RATE		353000000

/* The IPQ5424 has three physical ports and the port of the EDMA. */
#define IPQ5424_PPE_PORT_MAX	4
#define IPQ5424_PPE_CLK_RATE	375000000

/* ICC clocks for enabling PPE device. The avg_bw and peak_bw with value 0
 * will be updated by the clock rate of PPE.
 */
static const struct icc_bulk_data ipq9574_ppe_icc_data[] = {
	{
		.name = "ppe",
		.avg_bw = 0,
		.peak_bw = 0,
	},
	{
		.name = "ppe_cfg",
		.avg_bw = 0,
		.peak_bw = 0,
	},
	{
		.name = "qos_gen",
		.avg_bw = 6000,
		.peak_bw = 6000,
	},
	{
		.name = "timeout_ref",
		.avg_bw = 6000,
		.peak_bw = 6000,
	},
	{
		.name = "nssnoc_memnoc",
		.avg_bw = 533333,
		.peak_bw = 533333,
	},
	{
		.name = "memnoc_nssnoc",
		.avg_bw = 533333,
		.peak_bw = 533333,
	},
	{
		.name = "memnoc_nssnoc_1",
		.avg_bw = 533333,
		.peak_bw = 533333,
	},
};

static const struct regmap_range ppe_readable_ranges[] = {
	regmap_reg_range(0x0, 0x1ff),		/* Global */
	regmap_reg_range(0x400, 0x5ff),		/* LPI CSR */
	regmap_reg_range(0x1000, 0x11ff),	/* GMAC0 */
	regmap_reg_range(0x1200, 0x13ff),	/* GMAC1 */
	regmap_reg_range(0x1400, 0x15ff),	/* GMAC2 */
	regmap_reg_range(0x1600, 0x17ff),	/* GMAC3 */
	regmap_reg_range(0x1800, 0x19ff),	/* GMAC4 */
	regmap_reg_range(0x1a00, 0x1bff),	/* GMAC5 */
	regmap_reg_range(0xb000, 0xefff),	/* PRX CSR */
	regmap_reg_range(0xf000, 0x1efff),	/* IPE */
	regmap_reg_range(0x20000, 0x5ffff),	/* PTX CSR */
	regmap_reg_range(0x60000, 0x9ffff),	/* IPE L2 CSR */
	regmap_reg_range(0xb0000, 0xeffff),	/* IPO CSR */
	regmap_reg_range(0x100000, 0x17ffff),	/* IPE PC */
	regmap_reg_range(0x180000, 0x1bffff),	/* PRE IPO CSR */
	regmap_reg_range(0x1d0000, 0x1dffff),	/* Tunnel parser */
	regmap_reg_range(0x1e0000, 0x1effff),	/* Ingress parse */
	regmap_reg_range(0x200000, 0x2fffff),	/* IPE L3 */
	regmap_reg_range(0x300000, 0x3fffff),	/* IPE tunnel */
	regmap_reg_range(0x400000, 0x4fffff),	/* Scheduler */
	regmap_reg_range(0x500000, 0x503fff),	/* XGMAC0 */
	regmap_reg_range(0x504000, 0x507fff),	/* XGMAC1 */
	regmap_reg_range(0x508000, 0x50bfff),	/* XGMAC2 */
	regmap_reg_range(0x50c000, 0x50ffff),	/* XGMAC3 */
	regmap_reg_range(0x510000, 0x513fff),	/* XGMAC4 */
	regmap_reg_range(0x514000, 0x517fff),	/* XGMAC5 */
	regmap_reg_range(0x600000, 0x6fffff),	/* BM */
	regmap_reg_range(0x800000, 0x9fffff),	/* QM */
	regmap_reg_range(0xb00000, 0xbef800),	/* EDMA */
};

static const struct regmap_access_table ppe_reg_table = {
	.yes_ranges = ppe_readable_ranges,
	.n_yes_ranges = ARRAY_SIZE(ppe_readable_ranges),
};

/* The IPQ5424 has three Ethernet ports, and the register space of the GMACs
 * and XGMACs after the third is not there.
 */
static const struct regmap_range ipq5424_ppe_reserved_ranges[] = {
	regmap_reg_range(0x1600, 0x17ff),	/* GMAC3 */
	regmap_reg_range(0x1800, 0x19ff),	/* GMAC4 */
	regmap_reg_range(0x1a00, 0x1bff),	/* GMAC5 */
	regmap_reg_range(0x50c000, 0x50ffff),	/* XGMAC3 */
	regmap_reg_range(0x510000, 0x513fff),	/* XGMAC4 */
	regmap_reg_range(0x514000, 0x517fff),	/* XGMAC5 */
};

static const struct regmap_access_table ipq5424_ppe_reg_table = {
	.yes_ranges = ppe_readable_ranges,
	.n_yes_ranges = ARRAY_SIZE(ppe_readable_ranges),
	.no_ranges = ipq5424_ppe_reserved_ranges,
	.n_no_ranges = ARRAY_SIZE(ipq5424_ppe_reserved_ranges),
};

static const struct regmap_config regmap_config_ipq5424 = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.rd_table = &ipq5424_ppe_reg_table,
	.wr_table = &ipq5424_ppe_reg_table,
	.max_register = 0xbef800,
};

/* The interconnect paths of the IPQ5424. */
static const struct icc_bulk_data ipq5424_ppe_icc_data[] = {
	{
		.name = "ppe",
		.avg_bw = 0,
		.peak_bw = 0,
	},
	{
		.name = "ppe_cfg",
		.avg_bw = 0,
		.peak_bw = 0,
	},
	{
		.name = "nssnoc_ce_axi",
		.avg_bw = 0,
		.peak_bw = 0,
	},
	{
		.name = "nssnoc_ce_apb",
		.avg_bw = 0,
		.peak_bw = 0,
	},
	{
		.name = "nssnoc_nss_csr",
		.avg_bw = 100000,
		.peak_bw = 100000,
	},
};

static const struct regmap_config regmap_config_ipq9574 = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.rd_table = &ppe_reg_table,
	.wr_table = &ppe_reg_table,
	.max_register = 0xbef800,
};

static const struct ppe_mac_data ppe_ipq9574_mac_data = {
	.xgmac_addr = 0x500000,
	.xgmac_stride = 0x4000,
	.xgmac_first_port = 1,
	.xgmac_ports = GENMASK(6, 1),
	.mux = PPE_MAC_MUX_IPQ9574,
	.reset_delay_ms = 10,
	.xgmac_init = true,
	.mib = true,
	.bm_flow_control = true,
};

static const struct ppe_of_data ppe_ipq9574_data = {
	.type = PPE_APPE,
	.caps = PPE_CAP_SERVCODE | PPE_CAP_PORT_RX_CNT,
	.regs = &ppe_appe_regs,
	.clk_rate = PPE_CLK_RATE,
	.num_ports = PPE_PORT_MAX,
	.regmap_config = &regmap_config_ipq9574,
	.icc_data = ipq9574_ppe_icc_data,
	.num_icc_paths = ARRAY_SIZE(ipq9574_ppe_icc_data),
	.config = &ppe_ipq9574_config,
	.mac = &ppe_ipq9574_mac_data,
	.edma_gen = EDMA_V2,
	.edma_tag_mode = EDMA_TAG_NONE,
	.edma_data = &edmav2_ipq9574_data,
};

static const struct ppe_mac_data ppe_ipq5424_mac_data = {
	.xgmac_addr = 0x500000,
	.xgmac_stride = 0x4000,
	.xgmac_first_port = 1,
	.xgmac_ports = GENMASK(3, 1),
	.mux = PPE_MAC_MUX_IPQ9574,
	.reset_delay_ms = 10,
	.xgmac_init = true,
	.mib = true,
	.bm_flow_control = true,
};

static const struct ppe_of_data ppe_ipq5424_data = {
	.type = PPE_APPE,
	.caps = PPE_CAP_SERVCODE | PPE_CAP_PORT_RX_CNT,
	.regs = &ppe_appe_regs,
	.clk_rate = IPQ5424_PPE_CLK_RATE,
	.num_ports = IPQ5424_PPE_PORT_MAX,
	.regmap_config = &regmap_config_ipq5424,
	.icc_data = ipq5424_ppe_icc_data,
	.num_icc_paths = ARRAY_SIZE(ipq5424_ppe_icc_data),
	.config = &ppe_ipq5424_config,
	.mac = &ppe_ipq5424_mac_data,
	.edma_gen = EDMA_V2,
	.edma_tag_mode = EDMA_TAG_NONE,
	.edma_data = &edmav2_ipq5424_data,
};

/* IPQ6018 has no interconnects. Its ports 0 to 5 are the CPU port and five
 * physical ports, and port 6 is an internal loopback port.
 */
static const struct regmap_range ipq6018_ppe_readable_ranges[] = {
	regmap_reg_range(0x0, 0x1ff),		/* Global */
	regmap_reg_range(0x400, 0x5ff),		/* LPI CSR */
	regmap_reg_range(0x1000, 0x11ff),	/* GMAC0 */
	regmap_reg_range(0x1200, 0x13ff),	/* GMAC1 */
	regmap_reg_range(0x1400, 0x15ff),	/* GMAC2 */
	regmap_reg_range(0x1600, 0x17ff),	/* GMAC3 */
	regmap_reg_range(0x1800, 0x19ff),	/* GMAC4 */
	regmap_reg_range(0x1a00, 0x1bff),	/* GMAC5 */
	regmap_reg_range(0x1c00, 0x1dff),	/* GMAC6, loopback */
	regmap_reg_range(0x3000, 0x3fff),	/* XGMAC0 */
	regmap_reg_range(0xb000, 0xefff),	/* PRX CSR */
	regmap_reg_range(0xf000, 0x1efff),	/* IPE */
	regmap_reg_range(0x20000, 0x5ffff),	/* PTX CSR */
	regmap_reg_range(0x60000, 0x9ffff),	/* IPE L2 CSR */
	regmap_reg_range(0xb0000, 0xeffff),	/* IPO CSR */
	regmap_reg_range(0x100000, 0x17ffff),	/* IPE PC */
	regmap_reg_range(0x180000, 0x1bffff),	/* PRE IPO CSR */
	regmap_reg_range(0x1d0000, 0x1dffff),	/* Tunnel parser */
	regmap_reg_range(0x1e0000, 0x1effff),	/* Ingress parse */
	regmap_reg_range(0x200000, 0x2fffff),	/* IPE L3 */
	regmap_reg_range(0x300000, 0x3fffff),	/* IPE tunnel */
	regmap_reg_range(0x400000, 0x4fffff),	/* Scheduler */
	regmap_reg_range(0x600000, 0x6fffff),	/* BM */
	regmap_reg_range(0x800000, 0x9fffff),	/* QM */
	regmap_reg_range(0xb00000, 0xbaffff),	/* EDMA */
};

static const struct regmap_access_table ipq6018_ppe_reg_table = {
	.yes_ranges = ipq6018_ppe_readable_ranges,
	.n_yes_ranges = ARRAY_SIZE(ipq6018_ppe_readable_ranges),
};

static const struct regmap_config regmap_config_ipq6018 = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.rd_table = &ipq6018_ppe_reg_table,
	.wr_table = &ipq6018_ppe_reg_table,
	.max_register = 0xbafffc,
};

/* The IPQ8074 has the registers of the IPQ6018 and a second XGMAC at 0x7000.
 * Its ports 1 to 6 are physical ports and port 7 is an internal loopback port.
 */
static const struct regmap_range ipq8074_ppe_readable_ranges[] = {
	regmap_reg_range(0x0, 0x1ff),		/* Global */
	regmap_reg_range(0x400, 0x5ff),		/* LPI CSR */
	regmap_reg_range(0x1000, 0x1dff),	/* GMAC0 to GMAC6 */
	regmap_reg_range(0x3000, 0x3fff),	/* XGMAC0 */
	regmap_reg_range(0x7000, 0x7fff),	/* XGMAC1 */
	regmap_reg_range(0xb000, 0xefff),	/* PRX CSR */
	regmap_reg_range(0xf000, 0x1efff),	/* IPE */
	regmap_reg_range(0x20000, 0x5ffff),	/* PTX CSR */
	regmap_reg_range(0x60000, 0x9ffff),	/* IPE L2 CSR */
	regmap_reg_range(0xb0000, 0xeffff),	/* IPO CSR */
	regmap_reg_range(0x100000, 0x17ffff),	/* IPE PC */
	regmap_reg_range(0x180000, 0x1bffff),	/* PRE IPO CSR */
	regmap_reg_range(0x1d0000, 0x1dffff),	/* Tunnel parser */
	regmap_reg_range(0x1e0000, 0x1effff),	/* Ingress parse */
	regmap_reg_range(0x200000, 0x2fffff),	/* IPE L3 */
	regmap_reg_range(0x300000, 0x3fffff),	/* IPE tunnel */
	regmap_reg_range(0x400000, 0x4fffff),	/* Scheduler */
	regmap_reg_range(0x600000, 0x6fffff),	/* BM */
	regmap_reg_range(0x800000, 0x9fffff),	/* QM */
	regmap_reg_range(0xb00000, 0xbaffff),	/* EDMA */
};

static const struct regmap_access_table ipq8074_ppe_reg_table = {
	.yes_ranges = ipq8074_ppe_readable_ranges,
	.n_yes_ranges = ARRAY_SIZE(ipq8074_ppe_readable_ranges),
};

static const struct regmap_config regmap_config_ipq8074 = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.rd_table = &ipq8074_ppe_reg_table,
	.wr_table = &ipq8074_ppe_reg_table,
	.max_register = 0xbafffc,
};

static const struct ppe_mac_data ppe_ipq8074_mac_data = {
	.xgmac_addr = 0x3000,
	.xgmac_stride = 0x4000,
	.xgmac_first_port = 5,
	.xgmac_ports = BIT(5) | BIT(6),
	.mux = PPE_MAC_MUX_IPQ8074,
	.reset_delay_ms = 150,
	.gmac_2500 = true,
	.xgmac_lpbk_drain = true,
};

static const struct ppe_of_data ppe_ipq8074_data = {
	.type = PPE_HPPE,
	.regs = &ppe_hppe_regs,
	.clk_rate = 300000000,
	.num_ports = 8,
	.regmap_config = &regmap_config_ipq8074,
	.config = &ppe_ipq8074_config,
	.mac = &ppe_ipq8074_mac_data,
	.loopback_port = 7,
	.edma_gen = EDMA_V1,
	.edma_tag_mode = EDMA_TAG_DSA,
	.edma_data = &edmav1_ipq8074_data,
};

static const struct ppe_mac_data ppe_ipq6018_mac_data = {
	.xgmac_addr = 0x3000,
	.xgmac_stride = 0x4000,
	.xgmac_first_port = 5,
	.xgmac_ports = BIT(5),
	.mux = PPE_MAC_MUX_IPQ6018,
	.reset_delay_ms = 150,
	.gmac_2500 = true,
	.xgmac_lpbk_drain = true,
	.mib = true,
};

static const struct ppe_of_data ppe_ipq6018_data = {
	.type = PPE_CPPE,
	.regs = &ppe_hppe_regs,
	.clk_rate = 300000000,
	.num_ports = 7,
	.regmap_config = &regmap_config_ipq6018,
	.config = &ppe_ipq6018_config,
	.mac = &ppe_ipq6018_mac_data,
	.loopback_port = 6,
	.edma_gen = EDMA_V1,
	.edma_tag_mode = EDMA_TAG_DSA,
	.edma_data = &edmav1_ipq6018_data,
};

static int ppe_clock_init_and_reset(struct ppe_device *ppe_dev)
{
	const struct icc_bulk_data *icc_data = ppe_dev->data->icc_data;
	unsigned long ppe_rate = ppe_dev->clk_rate;
	struct device *dev = ppe_dev->dev;
	struct reset_control *rstc;
	struct clk_bulk_data *clks;
	struct clk *clk;
	int ret, i;

	for (i = 0; i < ppe_dev->num_icc_paths; i++) {
		ppe_dev->icc_paths[i].name = icc_data[i].name;
		ppe_dev->icc_paths[i].avg_bw = icc_data[i].avg_bw ? :
					       Bps_to_icc(ppe_rate);

		/* PPE does not have an explicit peak bandwidth requirement,
		 * so set the peak bandwidth to be equal to the average
		 * bandwidth.
		 */
		ppe_dev->icc_paths[i].peak_bw = icc_data[i].peak_bw ? :
						Bps_to_icc(ppe_rate);
	}

	ret = devm_of_icc_bulk_get(dev, ppe_dev->num_icc_paths,
				   ppe_dev->icc_paths);
	if (ret)
		return ret;

	ret = icc_bulk_set_bw(ppe_dev->num_icc_paths, ppe_dev->icc_paths);
	if (ret)
		return ret;

	/* The PPE clocks have a common parent clock. Setting the clock
	 * rate of "ppe" ensures the clock rate of all PPE clocks is
	 * configured to the same rate.
	 */
	clk = devm_clk_get(dev, "ppe");
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	ret = clk_set_rate(clk, ppe_rate);
	if (ret)
		return ret;

	ret = devm_clk_bulk_get_all_enabled(dev, &clks);
	if (ret < 0)
		return ret;

	/* Reset the PPE. */
	rstc = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(rstc))
		return PTR_ERR(rstc);

	ret = reset_control_assert(rstc);
	if (ret)
		return ret;

	/* The delay 10 ms of assert is necessary for resetting PPE. */
	usleep_range(10000, 11000);

	return reset_control_deassert(rstc);
}

/* A device tree that has no EDMA node is a PPE that only forwards in
 * hardware.
 */
static bool ppe_has_child(struct device *dev, const char *name)
{
	struct device_node *np = of_get_child_by_name(dev->of_node, name);

	of_node_put(np);

	return np;
}

/* How the ports reach the network stack. Ports that are available are
 * netdevs, and without any port node the PPE only forwards in hardware.
 */
enum ppe_port_model {
	PPE_PORT_MODEL_NONE,
	PPE_PORT_MODEL_DIRECT,
};

static enum ppe_port_model ppe_port_model_get(struct device *dev)
{
	enum ppe_port_model model = PPE_PORT_MODEL_NONE;
	struct device_node *ports_np;

	ports_np = of_get_child_by_name(dev->of_node, "ethernet-ports");
	if (!ports_np)
		return model;

	for_each_available_child_of_node_scoped(ports_np, port_np)
		model = PPE_PORT_MODEL_DIRECT;

	of_node_put(ports_np);

	return model;
}

static int qcom_ppe_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct ppe_of_data *data;
	struct ppe_device *ppe_dev;
	void __iomem *base;
	int ret, num_icc;

	data = device_get_match_data(dev);
	if (!data)
		return -ENODEV;

	num_icc = data->num_icc_paths;
	ppe_dev = devm_kzalloc(dev, struct_size(ppe_dev, icc_paths, num_icc),
			       GFP_KERNEL);
	if (!ppe_dev)
		return -ENOMEM;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return dev_err_probe(dev, PTR_ERR(base), "PPE ioremap failed\n");

	ppe_dev->regmap = devm_regmap_init_mmio(dev, base, data->regmap_config);
	if (IS_ERR(ppe_dev->regmap))
		return dev_err_probe(dev, PTR_ERR(ppe_dev->regmap),
				     "PPE initialize regmap failed\n");
	ppe_dev->dev = dev;
	ppe_dev->data = data;
	spin_lock_init(&ppe_dev->fdb_lock);
	ppe_dev->clk_rate = data->clk_rate;
	ppe_dev->num_ports = data->num_ports;
	ppe_dev->num_icc_paths = num_icc;

	ret = ppe_clock_init_and_reset(ppe_dev);
	if (ret)
		return dev_err_probe(dev, ret, "PPE clock config failed\n");

	ret = ppe_hw_config(ppe_dev);
	if (ret)
		return dev_err_probe(dev, ret, "PPE HW config failed\n");

	if (data->mac) {
		ret = ppe_mac_init(ppe_dev);
		if (ret)
			return dev_err_probe(dev, ret, "PPE MAC init failed\n");
	}

	if (data->edma_gen != EDMA_NONE && ppe_has_child(dev, "ethernet-dma")) {
		enum ppe_port_model model = ppe_port_model_get(dev);
		struct edma_config edma_cfg = {
			.tag_mode = EDMA_TAG_NONE,
		};

		ret = edma_init(ppe_dev, &edma_cfg, &ppe_dev->edma);
		if (ret)
			return dev_err_probe(dev, ret, "EDMA init failed\n");

		if (model == PPE_PORT_MODEL_DIRECT) {
			ret = ppe_port_init(ppe_dev);
			if (ret) {
				edma_fini(ppe_dev->edma);
				return dev_err_probe(dev, ret,
						     "port model init failed\n");
			}
		}
	}

	ppe_debugfs_setup(ppe_dev);
	platform_set_drvdata(pdev, ppe_dev);

	return 0;
}

static void qcom_ppe_remove(struct platform_device *pdev)
{
	struct ppe_device *ppe_dev;

	ppe_dev = platform_get_drvdata(pdev);
	ppe_debugfs_teardown(ppe_dev);
	ppe_port_deinit(ppe_dev);
	if (ppe_dev->edma)
		edma_fini(ppe_dev->edma);
}

static const struct of_device_id qcom_ppe_of_match[] = {
	{ .compatible = "qcom,ipq5424-ppe", .data = &ppe_ipq5424_data },
	{ .compatible = "qcom,ipq9574-ppe", .data = &ppe_ipq9574_data },
	{ .compatible = "qcom,ipq6018-ppe", .data = &ppe_ipq6018_data },
	{ .compatible = "qcom,ipq8074-ppe", .data = &ppe_ipq8074_data },
	{}
};
MODULE_DEVICE_TABLE(of, qcom_ppe_of_match);

static struct platform_driver qcom_ppe_driver = {
	.driver = {
		.name = "qcom_ppe",
		.of_match_table = qcom_ppe_of_match,
	},
	.probe	= qcom_ppe_probe,
	.remove = qcom_ppe_remove,
};
module_platform_driver(qcom_ppe_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Qualcomm Technologies, Inc. IPQ PPE driver");
