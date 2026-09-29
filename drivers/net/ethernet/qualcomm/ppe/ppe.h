/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __PPE_H__
#define __PPE_H__

#include <linux/compiler.h>
#include <linux/interconnect.h>

#include "edma.h"

struct device;
struct regmap;
struct regmap_config;
struct dentry;
struct ppe_config_data;
struct edma;

/**
 * struct ppe_of_data - Per-SoC PPE data, selected by the compatible string.
 * @clk_rate: PPE clock rate.
 * @num_ports: Number of PPE ports.
 * @regmap_config: Register map configuration.
 * @icc_data: Interconnect path descriptions.
 * @num_icc_paths: Number of interconnect paths.
 * @config: BM, QM and scheduler tables, owned by ppe_config.c.
 * @edma_gen: EDMA generation, EDMA_NONE if the SoC has no EDMA support.
 * @edma_tag_mode: Frame format on the EDMA conduit.
 * @edma_data: Data of the SoC for the EDMA implementation.
 */
struct ppe_of_data {
	unsigned long clk_rate;
	unsigned int num_ports;
	const struct regmap_config *regmap_config;
	const struct icc_bulk_data *icc_data;
	unsigned int num_icc_paths;
	const struct ppe_config_data *config;
	enum edma_gen edma_gen;
	enum edma_tag_mode edma_tag_mode;
	const void *edma_data;
};

/**
 * struct ppe_device - PPE device private data.
 * @dev: PPE device structure.
 * @regmap: PPE register map.
 * @data: Per-SoC data.
 * @edma: EDMA instance, NULL if the SoC has no EDMA support.
 * @clk_rate: PPE clock rate.
 * @num_ports: Number of PPE ports.
 * @debugfs_root: Debugfs root entry.
 * @num_icc_paths: Number of interconnect paths.
 * @icc_paths: Interconnect path array.
 *
 * PPE device is the instance of PPE hardware, which is used to
 * configure PPE packet process modules such as BM (buffer management),
 * QM (queue management), and scheduler.
 */
struct ppe_device {
	struct device *dev;
	struct regmap *regmap;
	const struct ppe_of_data *data;
	struct edma *edma;
	unsigned long clk_rate;
	unsigned int num_ports;
	struct dentry *debugfs_root;
	unsigned int num_icc_paths;
	struct icc_bulk_data icc_paths[] __counted_by(num_icc_paths);
};
#endif
