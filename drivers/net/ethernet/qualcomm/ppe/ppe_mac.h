/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __PPE_MAC_H__
#define __PPE_MAC_H__

#include <linux/phy.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>

struct clk;
struct device_node;
struct ppe_device;
struct ethtool_stats;
struct phylink_pcs;
struct reset_control;
struct rtnl_link_stats64;

/* The highest number of ports that have a MAC. */
#define PPE_MAC_MAX_PORTS	8

/**
 * enum ppe_mac_type - MAC that is connected to the PCS of a port.
 * @PPE_MAC_TYPE_GMAC: GMAC
 * @PPE_MAC_TYPE_XGMAC: XGMAC
 */
enum ppe_mac_type {
	PPE_MAC_TYPE_GMAC,
	PPE_MAC_TYPE_XGMAC,
};

/**
 * enum ppe_mac_mux - Layout of the port mux register.
 * @PPE_MAC_MUX_IPQ6018: Port 5 selects the PCS and the MAC, and port 3 can
 *	use channel 4 of the first PCS.
 * @PPE_MAC_MUX_IPQ9574: Each port selects the MAC, and port 5 selects
 *	the PCS.
 */
enum ppe_mac_mux {
	PPE_MAC_MUX_IPQ6018,
	PPE_MAC_MUX_IPQ9574,
};

/**
 * struct ppe_mac_data - Per-SoC data of the port MACs.
 * @xgmac_addr: Address of the XGMAC of the port @xgmac_first_port
 * @xgmac_stride: Distance between the XGMACs of two ports
 * @xgmac_first_port: The port that has the first XGMAC
 * @xgmac_ports: Bit for each port that has an XGMAC
 * @mux: Layout of the port mux register
 * @reset_delay_ms: Time that the reset of a MAC is asserted for
 * @gmac_2500: The GMAC handles 2500BASE-X without in-band autonegotiation
 * @xgmac_init: The XGMACs are set up at probe time, and not when the link
 *	is configured
 * @xgmac_lpbk_drain: The transmitter of an XGMAC is looped back into its
 *	egress path when the link goes down, so that the path is drained
 * @mib: The MACs have MIB counters
 * @bm_flow_control: The flow control of the buffer manager port follows the
 *	transmit pause of the MAC
 */
struct ppe_mac_data {
	u32 xgmac_addr;
	u32 xgmac_stride;
	u8 xgmac_first_port;
	unsigned long xgmac_ports;
	enum ppe_mac_mux mux;
	unsigned int reset_delay_ms;
	bool gmac_2500;
	bool xgmac_init;
	bool xgmac_lpbk_drain;
	bool mib;
	bool bm_flow_control;
};

/**
 * struct ppe_mac - MAC of a port.
 * @ppe_dev: PPE device
 * @np: Node of the port
 * @port: Port number
 * @type: MAC that the port uses
 * @clk_mac: Clock of the MAC
 * @clk_rx: Receive clock
 * @clk_tx: Transmit clock
 * @rst_mac: Reset of the MAC
 * @rst_rx: Reset of the receive path
 * @rst_tx: Reset of the transmit path
 * @gmib_read: Work that polls the GMAC MIB counters
 * @gmib_stats: Sum of the GMAC MIB counters
 * @gmib_stats_lock: Protects @gmib_stats
 */
struct ppe_mac {
	struct ppe_device *ppe_dev;
	struct device_node *np;
	unsigned int port;
	enum ppe_mac_type type;
	struct clk *clk_mac;
	struct clk *clk_rx;
	struct clk *clk_tx;
	struct reset_control *rst_mac;
	struct reset_control *rst_rx;
	struct reset_control *rst_tx;
	struct delayed_work gmib_read;
	u64 *gmib_stats;
	spinlock_t gmib_stats_lock; /* Protects gmib_stats */
};

struct phylink_pcs *ppe_mac_pcs_get(struct device_node *np);
int ppe_mac_init(struct ppe_device *ppe_dev);
struct ppe_mac *ppe_mac_get(struct ppe_device *ppe_dev, unsigned int port);
int ppe_mac_prepare(struct ppe_mac *mac, unsigned int mode,
		    phy_interface_t interface);
void ppe_mac_config(struct ppe_mac *mac, unsigned int mode,
		    phy_interface_t interface);
void ppe_mac_link_up(struct ppe_mac *mac, unsigned int mode,
		     phy_interface_t interface, int speed, int duplex,
		     bool tx_pause, bool rx_pause);
void ppe_mac_link_down(struct ppe_mac *mac, unsigned int mode,
		       phy_interface_t interface);
int ppe_mac_set_address(struct ppe_mac *mac, const u8 *addr);
u32 ppe_mac_xgmac_addr(const struct ppe_mac *mac);
void ppe_mac_lpbk_init(struct ppe_device *ppe_dev);
void ppe_mac_pcs_mux_init(struct ppe_device *ppe_dev);

int ppe_mac_stats_start(struct ppe_mac *mac);
void ppe_mac_stats_stop(struct ppe_mac *mac);
int ppe_mac_get_sset_count(struct ppe_mac *mac, int sset);
void ppe_mac_get_strings(struct ppe_mac *mac, u32 stringset, u8 *data);
void ppe_mac_get_ethtool_stats(struct ppe_mac *mac, u64 *data);
void ppe_mac_get_stats64(struct ppe_mac *mac, struct rtnl_link_stats64 *s);

#endif /* __PPE_MAC_H__ */
