/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __PPE_H__
#define __PPE_H__

#include <linux/bitmap.h>
#include <linux/bits.h>
#include <linux/compiler.h>
#include <linux/interconnect.h>
#include <linux/spinlock.h>

#include "edma.h"
#include "ppe_mac.h"

/* The size of the VSI table of the SoC with the largest one. */
#define PPE_VSI_MAX		64

struct device;
struct regmap;
struct regmap_config;
struct dentry;
struct ppe_config_data;
struct edma;

/**
 * enum ppe_type - Type of the PPE.
 * @PPE_HPPE: The base type.
 * @PPE_CPPE: Has the register map of the HPPE.
 * @PPE_APPE: Has a different register map and more features than the HPPE.
 * @PPE_MPPE: Has the register map of the APPE.
 */
enum ppe_type {
	PPE_HPPE,
	PPE_CPPE,
	PPE_APPE,
	PPE_MPPE,
};

/* Features that depend on the type of the PPE. */
#define PPE_CAP_PORT_RX_CNT	BIT(1)	/* Per-port RX counter tables. */

/**
 * struct ppe_regs - Register map of a PPE type.
 * @mru_mtu_tbl_addr: Base address of the per-port MRU and MTU table.
 * @vsi_tbl_addr: Base address of the VSI table.
 * @vsi_tbl_entries: Number of entries in the VSI tables.
 * @eg_bridge_config_addr: Address of the egress bridge configuration.
 * @port_eg_vlan_tbl_addr: Base address of the per-port egress VLAN table.
 * @l3_vp_port_tbl_addr: Base address of the L3 VP port table.
 * @l3_vp_port_tbl_words: Number of words in an entry of the L3 VP port table.
 * @eg_vsi_tag_addr: Base address of the egress VSI tag table.
 * @eg_vsi_tag_inc: Distance between the entries of the egress VSI tag table.
 * @eg_vsi_counter_tbl_addr: Base address of the egress VSI counter table.
 * @port_tx_counter_tbl_addr: Base address of the port TX counter table.
 * @vport_tx_counter_tbl_addr: Base address of the virtual port TX counter table.
 * @queue_tx_counter_tbl_addr: Base address of the queue TX counter table.
 */
struct ppe_regs {
	u32 mru_mtu_tbl_addr;
	u32 vsi_tbl_addr;
	unsigned int vsi_tbl_entries;
	u32 eg_bridge_config_addr;
	u32 port_eg_vlan_tbl_addr;
	u32 l3_vp_port_tbl_addr;
	unsigned int l3_vp_port_tbl_words;
	u32 eg_vsi_tag_addr;
	unsigned int eg_vsi_tag_inc;
	u32 eg_vsi_counter_tbl_addr;
	u32 port_tx_counter_tbl_addr;
	u32 vport_tx_counter_tbl_addr;
	u32 queue_tx_counter_tbl_addr;
};

extern const struct ppe_regs ppe_hppe_regs;
extern const struct ppe_regs ppe_appe_regs;

/**
 * struct ppe_of_data - Per-SoC PPE data, selected by the compatible string.
 * @type: Type of the PPE.
 * @caps: Features of the PPE type, PPE_CAP_*.
 * @regs: Register map of the PPE type.
 * @clk_rate: PPE clock rate.
 * @num_ports: Number of PPE ports.
 * @regmap_config: Register map configuration.
 * @icc_data: Interconnect path descriptions.
 * @num_icc_paths: Number of interconnect paths.
 * @config: BM, QM and scheduler tables, owned by ppe_config.c.
 * @mac: Data of the port MACs, NULL if the SoC has no port MACs.
 * @edma_gen: EDMA generation, EDMA_NONE if the SoC has no EDMA support.
 * @edma_tag_mode: Frame format on the EDMA conduit.
 * @edma_data: Data of the SoC for the EDMA implementation.
 */
struct ppe_of_data {
	enum ppe_type type;
	u32 caps;
	const struct ppe_regs *regs;
	unsigned long clk_rate;
	unsigned int num_ports;
	const struct regmap_config *regmap_config;
	const struct icc_bulk_data *icc_data;
	unsigned int num_icc_paths;
	const struct ppe_config_data *config;
	const struct ppe_mac_data *mac;
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
 * @port_netdev: Netdev of each port of the direct port model.
 * @macs: MAC of each port, NULL for a port without a MAC.
 * @vsi_bitmap: VSIs in use.
 * @fdb_lock: Serialises the operations of the FDB engine.
 * @fdb_cmd_id: Command id of the last write operation of the FDB engine.
 * @fdb_rd_cmd_id: Command id of the last read operation of the FDB engine.
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
	struct net_device *port_netdev[PPE_MAC_MAX_PORTS];
	DECLARE_BITMAP(vsi_bitmap, PPE_VSI_MAX);
	struct ppe_mac *macs[PPE_MAC_MAX_PORTS];
	/* Serialises the operations of the FDB engine. */
	spinlock_t fdb_lock;
	u32 fdb_cmd_id;
	u32 fdb_rd_cmd_id;
	struct dentry *debugfs_root;
	unsigned int num_icc_paths;
	struct icc_bulk_data icc_paths[] __counted_by(num_icc_paths);
};

static inline const struct ppe_regs *ppe_regs(const struct ppe_device *ppe_dev)
{
	return ppe_dev->data->regs;
}

static inline bool ppe_has_cap(const struct ppe_device *ppe_dev, u32 cap)
{
	return ppe_dev->data->caps & cap;
}
#endif
