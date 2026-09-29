// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

/* The MACs of the ports. A port has a GMAC and, on some ports, an XGMAC, and
 * a mux selects the one that the PCS of the port is connected to. The port
 * models call the functions here from their phylink operations.
 *
 * The GMAC and the XGMAC have different MIB counters. The XGMAC layout is
 * used for the statistics of a port, and the counters of the GMAC are added
 * to it, since a port can switch between the two MACs with its interface mode.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/ethtool.h>
#include <linux/of.h>
#include <linux/pcs/pcs-qca-uniphy.h>
#include <linux/phylink.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/rtnetlink.h>
#include <linux/workqueue.h>

#include "ppe.h"
#include "ppe_config.h"
#include "ppe_mac.h"
#include "ppe_regs.h"

/* Largest frame of a MAC, with the 4 bytes of the FCS. */
#define PPE_MAC_MAX_FRAME_SIZE		0x3000

/* The first buffer manager port of the MACs. */
#define PPE_BM_PORT_MAC_START		7

/* Poll interval time to poll GMAC MIBs for overflow protection,
 * the time should ensure that the 32bit GMAC packet counter
 * register would not overflow within this time at line rate
 * speed for 64B packet size.
 */
#define PPE_GMIB_POLL_INTERVAL_MS	120000

static const char * const ppe_mac_clk_names[] = { "mac", "rx", "tx" };

static struct ppe_mac *ppe_mac_from_ppe(struct ppe_device *ppe_dev, unsigned int port)
{
	return port < PPE_MAC_MAX_PORTS ? ppe_dev->macs[port] : NULL;
}

/**
 * ppe_mac_get - Get the MAC of a port.
 * @ppe_dev: PPE device.
 * @port: PPE port.
 *
 * Return: The MAC, or NULL if the port has none.
 */
struct ppe_mac *ppe_mac_get(struct ppe_device *ppe_dev, unsigned int port)
{
	return ppe_mac_from_ppe(ppe_dev, port);
}

u32 ppe_mac_xgmac_addr(const struct ppe_mac *mac)
{
	const struct ppe_mac_data *data = mac->ppe_dev->data->mac;

	return data->xgmac_addr +
	       (mac->port - data->xgmac_first_port) * data->xgmac_stride;
}

static bool ppe_mac_has_xgmac(const struct ppe_mac *mac)
{
	return mac->ppe_dev->data->mac->xgmac_ports & BIT(mac->port);
}

/* The MAC that a port uses for an interface mode. */
static enum ppe_mac_type ppe_mac_type_get(const struct ppe_mac *mac,
					  unsigned int mode,
					  phy_interface_t interface)
{
	const struct ppe_mac_data *data = mac->ppe_dev->data->mac;

	switch (interface) {
	case PHY_INTERFACE_MODE_2500BASEX:
		/* In-band autonegotiation is only supported by the XGMAC. */
		if (data->gmac_2500 && !phylink_autoneg_inband(mode))
			return PPE_MAC_TYPE_GMAC;

		return PPE_MAC_TYPE_XGMAC;
	case PHY_INTERFACE_MODE_USXGMII:
	case PHY_INTERFACE_MODE_10GBASER:
	case PHY_INTERFACE_MODE_10G_QXGMII:
		return PPE_MAC_TYPE_XGMAC;
	default:
		return PPE_MAC_TYPE_GMAC;
	}
}

static void ppe_mac_gmac_set(struct ppe_mac *mac, bool enable)
{
	regmap_update_bits(mac->ppe_dev->regmap,
			   PPE_PORT_GMAC_ADDR(mac->port) + PPE_GMAC_ENABLE_ADDR,
			   PPE_GMAC_TXEN | PPE_GMAC_RXEN,
			   enable ? PPE_GMAC_TXEN | PPE_GMAC_RXEN : 0);
}

static void ppe_mac_xgmac_set(struct ppe_mac *mac, bool enable)
{
	u32 base = ppe_mac_xgmac_addr(mac);

	regmap_update_bits(mac->ppe_dev->regmap, base + PPE_XGMAC_TX_CONFIG_ADDR,
			   PPE_XGMAC_TXEN, enable ? PPE_XGMAC_TXEN : 0);
	regmap_update_bits(mac->ppe_dev->regmap, base + PPE_XGMAC_RX_CONFIG_ADDR,
			   PPE_XGMAC_RXEN, enable ? PPE_XGMAC_RXEN : 0);
}

/* The receive side is turned off in the same write. Only the transmitter has
 * to drain, and the loop would otherwise learn the hosts behind the other
 * ports onto this port.
 */
static void ppe_mac_xgmac_lpbk_drain(struct ppe_mac *mac)
{
	u32 reg = ppe_mac_xgmac_addr(mac) + PPE_XGMAC_RX_CONFIG_ADDR;

	regmap_update_bits(mac->ppe_dev->regmap, reg,
			   PPE_XGMAC_LOOPBACK | PPE_XGMAC_RXEN, PPE_XGMAC_LOOPBACK);
	usleep_range(1000, 2000);
	regmap_clear_bits(mac->ppe_dev->regmap, reg, PPE_XGMAC_LOOPBACK);
}

static void ppe_mac_gmac_link_up(struct ppe_mac *mac, int speed, int duplex,
				 bool tx_pause, bool rx_pause)
{
	u32 reg = PPE_PORT_GMAC_ADDR(mac->port);
	u32 val;

	switch (speed) {
	case SPEED_100:
		val = PPE_GMAC_SPEED_100;
		break;
	case SPEED_2500:
	case SPEED_1000:
		val = PPE_GMAC_SPEED_1000;
		break;
	default:
		val = PPE_GMAC_SPEED_10;
		break;
	}
	regmap_update_bits(mac->ppe_dev->regmap, reg + PPE_GMAC_SPEED_ADDR,
			   PPE_GMAC_SPEED_M, val);

	val = 0;
	if (duplex == DUPLEX_FULL)
		val |= PPE_GMAC_DUPLEX_FULL;
	if (tx_pause)
		val |= PPE_GMAC_TXFCEN;
	if (rx_pause)
		val |= PPE_GMAC_RXFCEN;
	regmap_update_bits(mac->ppe_dev->regmap, reg + PPE_GMAC_ENABLE_ADDR,
			   PPE_GMAC_DUPLEX_FULL | PPE_GMAC_TXFCEN | PPE_GMAC_RXFCEN, val);
}

static void ppe_mac_xgmac_link_up(struct ppe_mac *mac, phy_interface_t interface,
				  int speed, bool tx_pause, bool rx_pause)
{
	u32 base = ppe_mac_xgmac_addr(mac);
	bool usx = interface == PHY_INTERFACE_MODE_USXGMII ||
		   interface == PHY_INTERFACE_MODE_10G_QXGMII;
	/* A speed that is not listed selects gigabit, since the caller
	 * enables the MAC after this and the speed has to be written.
	 */
	u32 val = PPE_XGMAC_SPEED_1000;

	switch (speed) {
	case SPEED_2500:
		val = usx ? PPE_XGMAC_SPEED_2500_USXGMII : PPE_XGMAC_SPEED_2500;
		break;
	case SPEED_5000:
		val = PPE_XGMAC_SPEED_5000;
		break;
	case SPEED_10000:
		val = usx ? PPE_XGMAC_SPEED_10000_USXGMII : PPE_XGMAC_SPEED_10000;
		break;
	default:
		break;
	}

	regmap_update_bits(mac->ppe_dev->regmap, base + PPE_XGMAC_TX_CONFIG_ADDR,
			   PPE_XGMAC_SPEED_M, val);

	regmap_set_bits(mac->ppe_dev->regmap, base + PPE_XGMAC_RX_CONFIG_ADDR,
			PPE_XGMAC_ACS | PPE_XGMAC_CST);

	regmap_update_bits(mac->ppe_dev->regmap, base + PPE_XGMAC_TX_FLOW_CTRL_ADDR,
			   PPE_XGMAC_TXFCEN, tx_pause ? PPE_XGMAC_TXFCEN : 0);

	regmap_update_bits(mac->ppe_dev->regmap, base + PPE_XGMAC_RX_FLOW_CTRL_ADDR,
			   PPE_XGMAC_RXFCEN, rx_pause ? PPE_XGMAC_RXFCEN : 0);
}

static void ppe_mac_xgmac_init(struct ppe_mac *mac)
{
	struct regmap *regmap = mac->ppe_dev->regmap;
	u32 base = ppe_mac_xgmac_addr(mac);

	regmap_update_bits(regmap, base + PPE_XGMAC_TX_CONFIG_ADDR,
			   PPE_XGMAC_TXEN | PPE_XGMAC_JD, PPE_XGMAC_JD);

	regmap_update_bits(regmap, base + PPE_XGMAC_RX_CONFIG_ADDR,
			   PPE_XGMAC_RX_CONFIG_MASK,
			   FIELD_PREP(PPE_XGMAC_GPSL_M, PPE_MAC_MAX_FRAME_SIZE) |
			   PPE_XGMAC_GPSLEN | PPE_XGMAC_CST | PPE_XGMAC_ACS);

	regmap_update_bits(regmap, base + PPE_XGMAC_WD_TIMEOUT_ADDR,
			   PPE_XGMAC_WD_TIMEOUT_MASK, PPE_XGMAC_WD_TIMEOUT_VAL);

	regmap_update_bits(regmap, base + PPE_XGMAC_PKT_FILTER_ADDR,
			   PPE_XGMAC_PKT_FILTER_MASK, PPE_XGMAC_PKT_FILTER_VAL);

	regmap_update_bits(regmap, base + PPE_XGMAC_TX_FLOW_CTRL_ADDR,
			   PPE_XGMAC_PAUSE_TIME_M,
			   FIELD_PREP(PPE_XGMAC_PAUSE_TIME_M,
				      FIELD_MAX(PPE_XGMAC_PAUSE_TIME_M)));

	/* Enable the counters and reset them. */
	if (mac->ppe_dev->data->mac->mib)
		regmap_update_bits(regmap, base + PPE_XGMAC_MMC_CTRL_ADDR,
				   PPE_XGMAC_MCF | PPE_XGMAC_CNTRST,
				   PPE_XGMAC_CNTRST);
}

/* Set the interface dependent part of the port mux before the PCS and the
 * MAC are configured.
 */
static void ppe_mac_mux_ipq6018(struct ppe_mac *mac, unsigned int mode,
				phy_interface_t interface)
{
	u32 mask, val = 0;

	if (mac->port != 5)
		return;

	mask = PPE_CPPE_PORT5_PCS_SEL | PPE_CPPE_PORT5_GMAC_SEL;
	switch (interface) {
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_1000BASEX:
		val = FIELD_PREP(PPE_CPPE_PORT5_PCS_SEL, PPE_CPPE_PORT5_PCS1_CH0);
		break;
	case PHY_INTERFACE_MODE_2500BASEX:
		val = FIELD_PREP(PPE_CPPE_PORT5_PCS_SEL, PPE_CPPE_PORT5_PCS1_CH0);
		if (ppe_mac_type_get(mac, mode, interface) == PPE_MAC_TYPE_XGMAC)
			val |= PPE_CPPE_PORT5_GMAC_SEL;
		break;
	case PHY_INTERFACE_MODE_10GBASER:
	case PHY_INTERFACE_MODE_USXGMII:
		val = FIELD_PREP(PPE_CPPE_PORT5_PCS_SEL, PPE_CPPE_PORT5_PCS1_CH0) |
		      PPE_CPPE_PORT5_GMAC_SEL;
		break;
	default:
		return;
	}

	regmap_update_bits(mac->ppe_dev->regmap, PPE_PORT_MUX_CTRL_ADDR, mask, val);
}

/* Port 5 connects to the first PCS in PSGMII mode and to the second one
 * otherwise.
 */
static void ppe_mac_mux_ipq9574(struct ppe_mac *mac, phy_interface_t interface)
{
	if (mac->port != 5)
		return;

	regmap_update_bits(mac->ppe_dev->regmap, PPE_PORT_MUX_CTRL_ADDR,
			   PPE_PORT5_SEL_PCS1,
			   interface == PHY_INTERFACE_MODE_PSGMII ?
			   0 : PPE_PORT5_SEL_PCS1);
}

/* Port 5 connects to the first PCS in PSGMII mode and to the second one
 * otherwise. The ports 5 and 6 select the GMAC or the XGMAC, and the PCS of
 * port 4 is the first one.
 */
static void ppe_mac_mux_ipq8074(struct ppe_mac *mac, unsigned int mode,
				phy_interface_t interface)
{
	bool gmac = ppe_mac_type_get(mac, mode, interface) == PPE_MAC_TYPE_GMAC;
	u32 mask, val;

	switch (mac->port) {
	case 5:
		mask = PPE_IPQ8074_PORT5_PCS_SEL | PPE_IPQ8074_PORT5_GMAC_SEL;
		val = FIELD_PREP(PPE_IPQ8074_PORT5_PCS_SEL,
				 interface == PHY_INTERFACE_MODE_PSGMII ?
				 PPE_IPQ8074_PORT5_PCS0 : PPE_IPQ8074_PORT5_PCS1);
		if (gmac)
			val |= PPE_IPQ8074_PORT5_GMAC_SEL;
		break;
	case 6:
		mask = PPE_IPQ8074_PORT6_PCS_SEL | PPE_IPQ8074_PORT6_GMAC_SEL;
		val = PPE_IPQ8074_PORT6_PCS_SEL;
		if (gmac)
			val |= PPE_IPQ8074_PORT6_GMAC_SEL;
		break;
	default:
		mask = PPE_IPQ8074_PORT4_PCS_SEL;
		val = PPE_IPQ8074_PORT4_PCS_SEL;
		break;
	}

	regmap_update_bits(mac->ppe_dev->regmap, PPE_PORT_MUX_CTRL_ADDR, mask, val);
}

/**
 * ppe_mac_prepare - Prepare the MAC and the mux for an interface mode.
 * @mac: MAC of the port.
 * @mode: Autonegotiation mode.
 * @interface: Interface mode.
 *
 * To be called from mac_prepare() of phylink.
 *
 * Return: 0.
 */
int ppe_mac_prepare(struct ppe_mac *mac, unsigned int mode,
		    phy_interface_t interface)
{
	switch (mac->ppe_dev->data->mac->mux) {
	case PPE_MAC_MUX_IPQ6018:
		ppe_mac_mux_ipq6018(mac, mode, interface);
		break;
	case PPE_MAC_MUX_IPQ9574:
		ppe_mac_mux_ipq9574(mac, interface);
		break;
	case PPE_MAC_MUX_IPQ8074:
		ppe_mac_mux_ipq8074(mac, mode, interface);
		break;
	}

	return 0;
}

static void ppe_mac_reset(struct ppe_mac *mac)
{
	struct reset_control *rsts[] = { mac->rst_mac, mac->rst_rx, mac->rst_tx };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(rsts); i++)
		if (rsts[i])
			reset_control_assert(rsts[i]);

	msleep(mac->ppe_dev->data->mac->reset_delay_ms);

	for (i = 0; i < ARRAY_SIZE(rsts); i++)
		if (rsts[i])
			reset_control_deassert(rsts[i]);
}

/**
 * ppe_mac_config - Configure the MAC for an interface mode.
 * @mac: MAC of the port.
 * @mode: Autonegotiation mode.
 * @interface: Interface mode.
 *
 * To be called from mac_config() of phylink. The MAC is reset and the mux
 * connects the MAC that the interface needs.
 */
void ppe_mac_config(struct ppe_mac *mac, unsigned int mode,
		    phy_interface_t interface)
{
	const struct ppe_mac_data *data = mac->ppe_dev->data->mac;
	enum ppe_mac_type type = ppe_mac_type_get(mac, mode, interface);

	if (type == PPE_MAC_TYPE_XGMAC && !ppe_mac_has_xgmac(mac))
		return;

	if (type == PPE_MAC_TYPE_XGMAC && !data->xgmac_init)
		ppe_mac_xgmac_init(mac);

	ppe_mac_reset(mac);

	if (data->mux == PPE_MAC_MUX_IPQ9574)
		regmap_update_bits(mac->ppe_dev->regmap, PPE_PORT_MUX_CTRL_ADDR,
				   PPE_PORT_SEL_XGMAC(mac->port),
				   type == PPE_MAC_TYPE_XGMAC ?
				   PPE_PORT_SEL_XGMAC(mac->port) : 0);

	mac->type = type;
}

static unsigned long ppe_mac_clk_rate(phy_interface_t interface, int speed)
{
	switch (interface) {
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_QSGMII:
	case PHY_INTERFACE_MODE_PSGMII:
	case PHY_INTERFACE_MODE_1000BASEX:
	case PHY_INTERFACE_MODE_2500BASEX:
		switch (speed) {
		case SPEED_10:
			return 2500000;
		case SPEED_100:
			return 25000000;
		case SPEED_2500:
			return 312500000;
		default:
			return 125000000;
		}
	case PHY_INTERFACE_MODE_USXGMII:
	case PHY_INTERFACE_MODE_10GBASER:
	case PHY_INTERFACE_MODE_10G_QXGMII:
		switch (speed) {
		case SPEED_10:
			return 1250000;
		case SPEED_100:
			return 12500000;
		case SPEED_2500:
			return 78125000;
		case SPEED_5000:
			return 156250000;
		case SPEED_10000:
			return 312500000;
		default:
			return 125000000;
		}
	default:
		return 125000000;
	}
}

/**
 * ppe_mac_link_up - Bring the MAC up.
 * @mac: MAC of the port.
 * @mode: Autonegotiation mode.
 * @interface: Interface mode.
 * @speed: Speed of the link.
 * @duplex: Duplex of the link.
 * @tx_pause: Transmit pause.
 * @rx_pause: Receive pause.
 *
 * To be called from mac_link_up() of phylink. The fabric is allowed to feed
 * the port when the MAC is up.
 */
void ppe_mac_link_up(struct ppe_mac *mac, unsigned int mode,
		     phy_interface_t interface, int speed, int duplex,
		     bool tx_pause, bool rx_pause)
{
	struct ppe_device *ppe_dev = mac->ppe_dev;
	const struct ppe_mac_data *data = ppe_dev->data->mac;
	enum ppe_mac_type type = ppe_mac_type_get(mac, mode, interface);
	unsigned long rate = ppe_mac_clk_rate(interface, speed);

	/* The ports without an XGMAC cannot run the faster interface modes. */
	if (type == PPE_MAC_TYPE_XGMAC && !ppe_mac_has_xgmac(mac))
		return;

	if (type == PPE_MAC_TYPE_GMAC) {
		if (data->mib && ppe_mac_stats_start(mac))
			return;

		ppe_mac_gmac_link_up(mac, speed, duplex, tx_pause, rx_pause);
	} else {
		ppe_mac_xgmac_link_up(mac, interface, speed, tx_pause, rx_pause);
	}

	if (mac->clk_rx)
		clk_set_rate(mac->clk_rx, rate);
	if (mac->clk_tx)
		clk_set_rate(mac->clk_tx, rate);

	if (type == PPE_MAC_TYPE_GMAC)
		ppe_mac_gmac_set(mac, true);
	else
		ppe_mac_xgmac_set(mac, true);

	if (data->bm_flow_control)
		regmap_update_bits(ppe_dev->regmap,
				   PPE_BM_PORT_FC_MODE_ADDR +
				   PPE_BM_PORT_FC_MODE_INC *
				   (mac->port + PPE_BM_PORT_MAC_START),
				   PPE_BM_PORT_FC_MODE_EN,
				   tx_pause ? PPE_BM_PORT_FC_MODE_EN : 0);

	/* The MAC is up, so the fabric may feed the port again. */
	ppe_port_txmac_set(ppe_dev, mac->port, true);
}

/**
 * ppe_mac_link_down - Stop the MAC.
 * @mac: MAC of the port.
 * @mode: Autonegotiation mode.
 * @interface: Interface mode.
 *
 * To be called from mac_link_down() of phylink. The fabric is stopped from
 * feeding the port before the MAC is stopped. Left on across a flap, the
 * fabric dequeues into a MAC that is being re-clocked, and the egress
 * scheduler of the port can stay latched.
 */
void ppe_mac_link_down(struct ppe_mac *mac, unsigned int mode,
		       phy_interface_t interface)
{
	const struct ppe_mac_data *data = mac->ppe_dev->data->mac;
	enum ppe_mac_type type = ppe_mac_type_get(mac, mode, interface);

	if (type == PPE_MAC_TYPE_XGMAC && !ppe_mac_has_xgmac(mac))
		return;

	ppe_port_txmac_set(mac->ppe_dev, mac->port, false);

	/* Let the egress path drain before the MAC goes. Frames that are
	 * left there when the link drops stop the queue manager.
	 */
	usleep_range(10000, 11000);

	if (type == PPE_MAC_TYPE_GMAC) {
		if (data->mib)
			ppe_mac_stats_stop(mac);

		ppe_mac_gmac_set(mac, false);
		return;
	}

	if (data->xgmac_lpbk_drain)
		ppe_mac_xgmac_lpbk_drain(mac);
	ppe_mac_xgmac_set(mac, false);
}

/**
 * ppe_mac_set_address - Set the MAC address of a port.
 * @mac: MAC of the port.
 * @addr: MAC address.
 *
 * Return: 0 on success, negative error code on failure.
 */
int ppe_mac_set_address(struct ppe_mac *mac, const u8 *addr)
{
	struct regmap *regmap = mac->ppe_dev->regmap;
	u32 reg = PPE_PORT_GMAC_ADDR(mac->port);
	int ret;

	ret = regmap_write(regmap, reg + PPE_GMAC_GOL_ADDR0_ADDR,
			   (addr[5] << 8) | addr[4]);
	if (ret)
		return ret;

	ret = regmap_write(regmap, reg + PPE_GMAC_GOL_ADDR1_ADDR,
			   (addr[0] << 24) | (addr[1] << 16) | (addr[2] << 8) |
			   addr[3]);
	if (ret)
		return ret;

	if (!ppe_mac_has_xgmac(mac))
		return 0;

	reg = ppe_mac_xgmac_addr(mac);
	ret = regmap_write(regmap, reg + PPE_XGMAC_ADDR0_H_ADDR,
			   (addr[5] << 8) | addr[4] | PPE_XGMAC_ADDR_EN);
	if (ret)
		return ret;

	return regmap_write(regmap, reg + PPE_XGMAC_ADDR0_L_ADDR,
			    (addr[3] << 24) | (addr[2] << 16) | (addr[1] << 8) |
			    addr[0]);
}

static void ppe_mac_hw_init(struct ppe_mac *mac)
{
	struct regmap *regmap = mac->ppe_dev->regmap;
	const struct ppe_mac_data *data = mac->ppe_dev->data->mac;
	u32 base = PPE_PORT_GMAC_ADDR(mac->port);

	/* The MAC is stopped until the link is up. */
	regmap_clear_bits(regmap, base + PPE_GMAC_ENABLE_ADDR,
			  PPE_GMAC_TXEN | PPE_GMAC_RXEN);

	regmap_update_bits(regmap, base + PPE_GMAC_CTRL_ADDR,
			   PPE_GMAC_MAXFRAME_SIZE_M | PPE_GMAC_CRS_SEL | PPE_GMAC_TX_THD_M,
			   FIELD_PREP(PPE_GMAC_MAXFRAME_SIZE_M, PPE_MAC_MAX_FRAME_SIZE) |
			   FIELD_PREP(PPE_GMAC_TX_THD_M, 1));

	regmap_update_bits(regmap, base + PPE_GMAC_DBG_CTRL_ADDR,
			   PPE_GMAC_HIGH_IPG_M, FIELD_PREP(PPE_GMAC_HIGH_IPG_M, 0xc));

	regmap_update_bits(regmap, base + PPE_GMAC_JUMBO_SIZE_ADDR,
			   PPE_GMAC_JUMBO_SIZE_M,
			   FIELD_PREP(PPE_GMAC_JUMBO_SIZE_M, PPE_MAC_MAX_FRAME_SIZE));

	/* Count and clear on read, so that the sum of the counters is kept
	 * here.
	 */
	if (data->mib) {
		regmap_set_bits(regmap, base + PPE_GMAC_MIB_CTRL_ADDR,
				PPE_GMAC_MIB_CTRL_MASK);
		regmap_clear_bits(regmap, base + PPE_GMAC_MIB_CTRL_ADDR,
				  PPE_GMAC_MIB_RST);
	}

	if (data->xgmac_init && ppe_mac_has_xgmac(mac))
		ppe_mac_xgmac_init(mac);
}

static void ppe_mac_clk_disable(void *clk)
{
	clk_disable_unprepare(clk);
	clk_put(clk);
}

static void ppe_mac_reset_put(void *rstc)
{
	reset_control_put(rstc);
}

static void ppe_mac_node_put(void *np)
{
	of_node_put(np);
}

/* A clock is optional, and a port that has none of the named one is not an
 * error. The lookup by name goes on in the parent nodes, which have clocks of
 * their own, so the name is looked for in the node first.
 */
static struct clk *ppe_mac_clk_get(struct device *dev, struct device_node *np,
				   const char *name)
{
	struct clk *clk;
	int ret;

	if (of_property_match_string(np, "clock-names", name) < 0)
		return NULL;

	clk = of_clk_get_by_name(np, name);
	if (IS_ERR(clk))
		return clk;

	ret = clk_prepare_enable(clk);
	if (ret) {
		clk_put(clk);
		return ERR_PTR(ret);
	}

	ret = devm_add_action_or_reset(dev, ppe_mac_clk_disable, clk);
	if (ret)
		return ERR_PTR(ret);

	return clk;
}

static struct reset_control *ppe_mac_reset_get(struct device *dev,
					       struct device_node *np,
					       const char *name)
{
	struct reset_control *rstc;
	int ret;

	rstc = of_reset_control_get_optional_exclusive(np, name);
	if (IS_ERR_OR_NULL(rstc))
		return rstc;

	ret = devm_add_action_or_reset(dev, ppe_mac_reset_put, rstc);
	if (ret)
		return ERR_PTR(ret);

	return rstc;
}

static int ppe_mac_probe_port(struct ppe_device *ppe_dev,
			      struct device_node *np, unsigned int port)
{
	struct device *dev = ppe_dev->dev;
	struct ppe_mac *mac;
	int ret;

	mac = devm_kzalloc(dev, sizeof(*mac), GFP_KERNEL);
	if (!mac)
		return -ENOMEM;

	mac->ppe_dev = ppe_dev;
	mac->port = port;
	mac->np = of_node_get(np);
	ret = devm_add_action_or_reset(dev, ppe_mac_node_put, mac->np);
	if (ret)
		return ret;

	mac->clk_mac = ppe_mac_clk_get(dev, np, ppe_mac_clk_names[0]);
	if (IS_ERR(mac->clk_mac))
		return PTR_ERR(mac->clk_mac);

	mac->clk_rx = ppe_mac_clk_get(dev, np, ppe_mac_clk_names[1]);
	if (IS_ERR(mac->clk_rx))
		return PTR_ERR(mac->clk_rx);

	mac->clk_tx = ppe_mac_clk_get(dev, np, ppe_mac_clk_names[2]);
	if (IS_ERR(mac->clk_tx))
		return PTR_ERR(mac->clk_tx);

	mac->rst_mac = ppe_mac_reset_get(dev, np, "mac");
	if (IS_ERR(mac->rst_mac))
		return PTR_ERR(mac->rst_mac);

	mac->rst_rx = ppe_mac_reset_get(dev, np, "rx");
	if (IS_ERR(mac->rst_rx))
		return PTR_ERR(mac->rst_rx);

	mac->rst_tx = ppe_mac_reset_get(dev, np, "tx");
	if (IS_ERR(mac->rst_tx))
		return PTR_ERR(mac->rst_tx);

	ppe_mac_hw_init(mac);
	ppe_dev->macs[port] = mac;

	return 0;
}

/**
 * ppe_mac_pcs_get - Get the PCS of a port.
 * @np: Node of the port.
 *
 * The "pcs-handle" property of the port names the UNIPHY and the channel of
 * the PCS.
 *
 * Return: The PCS, -ENODEV if the port has no "pcs-handle", or another error
 * pointer.
 */
struct phylink_pcs *ppe_mac_pcs_get(struct device_node *np)
{
	struct phylink_pcs *pcs = ERR_PTR(-EOPNOTSUPP);
	struct of_phandle_args args;

	if (of_parse_phandle_with_args(np, "pcs-handle", "#pcs-cells", 0, &args))
		return ERR_PTR(-ENODEV);

	if (IS_ENABLED(CONFIG_PCS_QCA_UNIPHY))
		pcs = qca_uniphy_pcs_get(args.np, args.args[0]);

	of_node_put(args.np);

	return pcs;
}

/**
 * ppe_mac_init - Set up the MACs of the ports.
 * @ppe_dev: PPE device.
 *
 * A port has a MAC when its node has clocks. The clocks are enabled for the
 * lifetime of the driver, and the rates of the receive and the transmit
 * clock follow the speed of the link.
 *
 * Return: 0 on success, negative error code on failure.
 */
int ppe_mac_init(struct ppe_device *ppe_dev)
{
	struct device_node *ports_np;
	int ret = 0;
	u32 port;

	if (!ppe_dev->data->mac)
		return -EOPNOTSUPP;

	/* A device tree without ports is a PPE that only forwards in hardware. */
	ports_np = of_get_child_by_name(ppe_dev->dev->of_node, "ethernet-ports");
	if (!ports_np)
		return 0;

	for_each_available_child_of_node_scoped(ports_np, port_np) {
		if (of_property_read_u32(port_np, "reg", &port) ||
		    port >= PPE_MAC_MAX_PORTS || port >= ppe_dev->num_ports)
			continue;

		if (!of_property_present(port_np, "clocks"))
			continue;

		ret = ppe_mac_probe_port(ppe_dev, port_np, port);
		if (ret)
			break;
	}

	of_node_put(ports_np);

	return ret;
}

#define PPE_MAC_MIB_DESC(_s, _o, _n)		\
	{					\
		.size = (_s),			\
		.offset = (_o),			\
		.name = (_n),			\
	}

/* PPE MAC MIB description */
struct ppe_mac_mib_info {
	u32 size;
	u32 offset;
	const char *name;
};

/* PPE GMAC MIB statistics type */
enum ppe_gmib_stats_type {
	gmib_rx_broadcast,
	gmib_rx_pause,
	gmib_rx_multicast,
	gmib_rx_fcserr,
	gmib_rx_alignerr,
	gmib_rx_runt,
	gmib_rx_frag,
	gmib_rx_jumbofcserr,
	gmib_rx_jumboalignerr,
	gmib_rx_pkt64,
	gmib_rx_pkt65to127,
	gmib_rx_pkt128to255,
	gmib_rx_pkt256to511,
	gmib_rx_pkt512to1023,
	gmib_rx_pkt1024to1518,
	gmib_rx_pkt1519tomax,
	gmib_rx_toolong,
	gmib_rx_bytes_g,
	gmib_rx_bytes_b,
	gmib_rx_unicast,
	gmib_tx_broadcast,
	gmib_tx_pause,
	gmib_tx_multicast,
	gmib_tx_underrun,
	gmib_tx_pkt64,
	gmib_tx_pkt65to127,
	gmib_tx_pkt128to255,
	gmib_tx_pkt256to511,
	gmib_tx_pkt512to1023,
	gmib_tx_pkt1024to1518,
	gmib_tx_pkt1519tomax,
	gmib_tx_bytes,
	gmib_tx_collisions,
	gmib_tx_abortcol,
	gmib_tx_multicol,
	gmib_tx_singlecol,
	gmib_tx_excdeffer,
	gmib_tx_deffer,
	gmib_tx_latecol,
	gmib_tx_unicast,
};

/* PPE XGMAC MIB statistics type */
enum ppe_xgmib_stats_type {
	xgmib_tx_bytes,
	xgmib_tx_frames,
	xgmib_tx_broadcast_g,
	xgmib_tx_multicast_g,
	xgmib_tx_pkt64,
	xgmib_tx_pkt65to127,
	xgmib_tx_pkt128to255,
	xgmib_tx_pkt256to511,
	xgmib_tx_pkt512to1023,
	xgmib_tx_pkt1024tomax,
	xgmib_tx_unicast,
	xgmib_tx_multicast,
	xgmib_tx_broadcast,
	xgmib_tx_underflow_err,
	xgmib_tx_bytes_g,
	xgmib_tx_frames_g,
	xgmib_tx_pause,
	xgmib_tx_vlan_g,
	xgmib_tx_lpi_usec,
	xgmib_tx_lpi_tran,
	xgmib_rx_frames,
	xgmib_rx_bytes,
	xgmib_rx_bytes_g,
	xgmib_rx_broadcast_g,
	xgmib_rx_multicast_g,
	xgmib_rx_crc_err,
	xgmib_rx_frag_err,
	xgmib_rx_jabber_err,
	xgmib_rx_undersize_g,
	xgmib_rx_oversize_g,
	xgmib_rx_pkt64,
	xgmib_rx_pkt65to127,
	xgmib_rx_pkt128to255,
	xgmib_rx_pkt256to511,
	xgmib_rx_pkt512to1023,
	xgmib_rx_pkt1024tomax,
	xgmib_rx_unicast_g,
	xgmib_rx_len_err,
	xgmib_rx_outofrange_err,
	xgmib_rx_pause,
	xgmib_rx_fifo_overflow,
	xgmib_rx_vlan,
	xgmib_rx_wdog_err,
	xgmib_rx_lpi_usec,
	xgmib_rx_lpi_tran,
	xgmib_rx_drop_frames,
	xgmib_rx_drop_bytes,
};

/* PPE GMAC MIB statistics description information */
static const struct ppe_mac_mib_info gmib_info[] = {
	PPE_MAC_MIB_DESC(4, PPE_GMAC_RXBROAD_ADDR, "rx_broadcast"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_RXPAUSE_ADDR, "rx_pause"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_RXMULTI_ADDR, "rx_multicast"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_RXFCSERR_ADDR, "rx_fcserr"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_RXALIGNERR_ADDR, "rx_alignerr"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_RXRUNT_ADDR, "rx_runt"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_RXFRAG_ADDR, "rx_frag"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_RXJUMBOFCSERR_ADDR, "rx_jumbofcserr"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_RXJUMBOALIGNERR_ADDR, "rx_jumboalignerr"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_RXPKT64_ADDR, "rx_pkt64"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_RXPKT65TO127_ADDR, "rx_pkt65to127"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_RXPKT128TO255_ADDR, "rx_pkt128to255"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_RXPKT256TO511_ADDR, "rx_pkt256to511"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_RXPKT512TO1023_ADDR, "rx_pkt512to1023"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_RXPKT1024TO1518_ADDR, "rx_pkt1024to1518"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_RXPKT1519TOX_ADDR, "rx_pkt1519tomax"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_RXTOOLONG_ADDR, "rx_toolong"),
	PPE_MAC_MIB_DESC(8, PPE_GMAC_RXBYTE_G_ADDR, "rx_bytes_g"),
	PPE_MAC_MIB_DESC(8, PPE_GMAC_RXBYTE_B_ADDR, "rx_bytes_b"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_RXUNI_ADDR, "rx_unicast"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_TXBROAD_ADDR, "tx_broadcast"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_TXPAUSE_ADDR, "tx_pause"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_TXMULTI_ADDR, "tx_multicast"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_TXUNDERRUN_ADDR, "tx_underrun"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_TXPKT64_ADDR, "tx_pkt64"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_TXPKT65TO127_ADDR, "tx_pkt65to127"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_TXPKT128TO255_ADDR, "tx_pkt128to255"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_TXPKT256TO511_ADDR, "tx_pkt256to511"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_TXPKT512TO1023_ADDR, "tx_pkt512to1023"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_TXPKT1024TO1518_ADDR, "tx_pkt1024to1518"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_TXPKT1519TOX_ADDR, "tx_pkt1519tomax"),
	PPE_MAC_MIB_DESC(8, PPE_GMAC_TXBYTE_ADDR, "tx_bytes"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_TXCOLLISIONS_ADDR, "tx_collisions"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_TXABORTCOL_ADDR, "tx_abortcol"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_TXMULTICOL_ADDR, "tx_multicol"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_TXSINGLECOL_ADDR, "tx_singlecol"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_TXEXCESSIVEDEFER_ADDR, "tx_excdeffer"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_TXDEFER_ADDR, "tx_deffer"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_TXLATECOL_ADDR, "tx_latecol"),
	PPE_MAC_MIB_DESC(4, PPE_GMAC_TXUNI_ADDR, "tx_unicast"),
};

/* PPE XGMAC MIB statistics description information */
static const struct ppe_mac_mib_info xgmib_info[] = {
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_TXBYTE_GB_ADDR, "tx_bytes"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_TXPKT_GB_ADDR, "tx_frames"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_TXBROAD_G_ADDR, "tx_broadcast_g"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_TXMULTI_G_ADDR, "tx_multicast_g"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_TXPKT64_GB_ADDR, "tx_pkt64"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_TXPKT65TO127_GB_ADDR, "tx_pkt65to127"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_TXPKT128TO255_GB_ADDR, "tx_pkt128to255"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_TXPKT256TO511_GB_ADDR, "tx_pkt256to511"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_TXPKT512TO1023_GB_ADDR, "tx_pkt512to1023"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_TXPKT1024TOMAX_GB_ADDR, "tx_pkt1024tomax"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_TXUNI_GB_ADDR, "tx_unicast"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_TXMULTI_GB_ADDR, "tx_multicast"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_TXBROAD_GB_ADDR, "tx_broadcast"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_TXUNDERFLOW_ERR_ADDR, "tx_underflow_err"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_TXBYTE_G_ADDR, "tx_bytes_g"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_TXPKT_G_ADDR, "tx_frames_g"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_TXPAUSE_ADDR, "tx_pause"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_TXVLAN_G_ADDR, "tx_vlan_g"),
	PPE_MAC_MIB_DESC(4, PPE_XGMAC_TXLPI_USEC_ADDR, "tx_lpi_usec"),
	PPE_MAC_MIB_DESC(4, PPE_XGMAC_TXLPI_TRAN_ADDR, "tx_lpi_tran"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_RXPKT_GB_ADDR, "rx_frames"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_RXBYTE_GB_ADDR, "rx_bytes"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_RXBYTE_G_ADDR, "rx_bytes_g"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_RXBROAD_G_ADDR, "rx_broadcast_g"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_RXMULTI_G_ADDR, "rx_multicast_g"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_RXCRC_ERR_ADDR, "rx_crc_err"),
	PPE_MAC_MIB_DESC(4, PPE_XGMAC_RXRUNT_ERR_ADDR, "rx_frag_err"),
	PPE_MAC_MIB_DESC(4, PPE_XGMAC_RXJABBER_ERR_ADDR, "rx_jabber_err"),
	PPE_MAC_MIB_DESC(4, PPE_XGMAC_RXUNDERSIZE_G_ADDR, "rx_undersize_g"),
	PPE_MAC_MIB_DESC(4, PPE_XGMAC_RXOVERSIZE_G_ADDR, "rx_oversize_g"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_RXPKT64_GB_ADDR, "rx_pkt64"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_RXPKT65TO127_GB_ADDR, "rx_pkt65to127"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_RXPKT128TO255_GB_ADDR, "rx_pkt128to255"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_RXPKT256TO511_GB_ADDR, "rx_pkt256to511"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_RXPKT512TO1023_GB_ADDR, "rx_pkt512to1023"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_RXPKT1024TOMAX_GB_ADDR, "rx_pkt1024tomax"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_RXUNI_G_ADDR, "rx_unicast_g"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_RXLEN_ERR_ADDR, "rx_len_err"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_RXOUTOFRANGE_ADDR, "rx_outofrange_err"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_RXPAUSE_ADDR, "rx_pause"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_RXFIFOOVERFLOW_ADDR, "rx_fifo_overflow"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_RXVLAN_GB_ADDR, "rx_vlan"),
	PPE_MAC_MIB_DESC(4, PPE_XGMAC_RXWATCHDOG_ERR_ADDR, "rx_wdog_err"),
	PPE_MAC_MIB_DESC(4, PPE_XGMAC_RXLPI_USEC_ADDR, "rx_lpi_usec"),
	PPE_MAC_MIB_DESC(4, PPE_XGMAC_RXLPI_TRAN_ADDR, "rx_lpi_tran"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_RXDISCARD_GB_ADDR, "rx_drop_frames"),
	PPE_MAC_MIB_DESC(8, PPE_XGMAC_RXDISCARDBYTE_GB_ADDR, "rx_drop_bytes"),
};

/* Get GMAC MIBs from registers and accumulate to PPE port GMIB stats array */
static void ppe_mac_gmib_update(struct ppe_mac *mac)
{
	struct ppe_device *ppe_dev = mac->ppe_dev;
	const struct ppe_mac_mib_info *mib;
	int port = mac->port;
	u32 reg, val;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(gmib_info); i++) {
		mib = &gmib_info[i];
		reg = PPE_PORT_GMAC_ADDR(port) + mib->offset;

		ret = regmap_read(ppe_dev->regmap, reg, &val);
		if (ret) {
			dev_warn(ppe_dev->dev, "PPE port GMIB read fail %d\n",
				 ret);
			continue;
		}

		mac->gmib_stats[i] += val;
		if (mib->size == 8) {
			ret = regmap_read(ppe_dev->regmap, reg + 4, &val);
			if (ret) {
				dev_warn(ppe_dev->dev,
					 "PPE port GMIB read fail %d\n", ret);
				continue;
			}

			mac->gmib_stats[i] += (u64)val << 32;
		}
	}
}

/* Polling task to read GMIB statistics to avoid GMIB 32bit register overflow */
static void ppe_mac_gmib_stats_poll(struct work_struct *work)
{
	struct ppe_mac *mac = container_of(work, struct ppe_mac,
						 gmib_read.work);
	spin_lock(&mac->gmib_stats_lock);
	ppe_mac_gmib_update(mac);
	spin_unlock(&mac->gmib_stats_lock);

	schedule_delayed_work(&mac->gmib_read,
			      msecs_to_jiffies(PPE_GMIB_POLL_INTERVAL_MS));
}

/**
 * ppe_mac_stats_start - Start the polling of the GMAC MIB counters.
 * @mac: MAC of the port.
 *
 * The GMAC counters have 32 bits and are added up regularly.
 *
 * Return: 0 on success, negative error code on failure.
 */
int ppe_mac_stats_start(struct ppe_mac *mac)
{
	struct ppe_device *ppe_dev = mac->ppe_dev;

	if (!mac->gmib_stats) {
		u64 *gstats;
		/* Allocate array memory to store GMIB statistics */
		gstats = devm_kzalloc(ppe_dev->dev,
				      sizeof(*gstats) * ARRAY_SIZE(gmib_info),
				      GFP_KERNEL);
		if (!gstats)
			return -ENOMEM;

		mac->gmib_stats = gstats;

		/* Init GMIB statistics polling work */
		spin_lock_init(&mac->gmib_stats_lock);
		INIT_DELAYED_WORK(&mac->gmib_read,
				  ppe_mac_gmib_stats_poll);
	}

	/* Start GMIB statistics polling work */
	schedule_delayed_work(&mac->gmib_read, 0);

	return 0;
}

/**
 * ppe_mac_stats_stop - Stop the polling of the GMAC MIB counters.
 * @mac: MAC of the port.
 */
void ppe_mac_stats_stop(struct ppe_mac *mac)
{
	if (mac->gmib_stats) {
		/* Stop GMIB statistics polling work */
		cancel_delayed_work_sync(&mac->gmib_read);
	}
}

/* Get the XGMAC MIB counter based on the specific MIB stats type */
static u64 ppe_mac_xgmib_get(struct ppe_mac *mac,
			     enum ppe_xgmib_stats_type xgmib_type)
{
	struct ppe_device *ppe_dev = mac->ppe_dev;
	const struct ppe_mac_mib_info *mib;
	u32 reg, val;
	u64 data = 0;
	int ret;

	/* A port without an XGMAC only has the GMAC counters. */
	if (!ppe_mac_has_xgmac(mac))
		return 0;

	mib = &xgmib_info[xgmib_type];
	reg = ppe_mac_xgmac_addr(mac) + mib->offset;

	ret = regmap_read(ppe_dev->regmap, reg, &val);
	if (ret) {
		dev_warn(ppe_dev->dev, "PPE port XGMIB read fail %d\n", ret);
		goto data_return;
	}

	data = val;
	if (mib->size == 8) {
		ret = regmap_read(ppe_dev->regmap, reg + 4, &val);
		if (ret) {
			dev_warn(ppe_dev->dev, "PPE port XGMIB read fail %d\n",
				 ret);
			goto data_return;
		}

		data |= (u64)val << 32;
	}

data_return:
	return data;
}

/**
 * ppe_mac_get_sset_count() - Get PPE port statistics string count
 * @mac: PPE port
 * @sset: string set ID
 *
 * Description: Get the MAC statistics string count for the PPE port
 * specified by @mac.
 *
 * Return: The count of the statistics string.
 */
int ppe_mac_get_sset_count(struct ppe_mac *mac, int sset)
{
	if (sset != ETH_SS_STATS || !mac->ppe_dev->data->mac->mib)
		return 0;

	/* The MAC type is invisible to the upper interface. The interface
	 * can switch between GMAC and XGMAC in different interface modes.
	 * Therefore, the unified XGMIB statistics format is used, and GMIB
	 * statistics will be merged into the XGMIB statistics.
	 */
	return ARRAY_SIZE(xgmib_info);
}

/**
 * ppe_mac_get_strings() - Get PPE port statistics strings
 * @mac: PPE port
 * @stringset: string set ID
 * @data: pointer to statistics strings
 *
 * Description: Get the MAC statistics stings for the PPE port
 * specified by @mac. The strings are stored in the buffer
 * indicated by @data which used in the ethtool ops.
 */
void ppe_mac_get_strings(struct ppe_mac *mac, u32 stringset, u8 *data)
{
	int i;

	if (stringset != ETH_SS_STATS || !mac->ppe_dev->data->mac->mib)
		return;

	for (i = 0; i < ARRAY_SIZE(xgmib_info); i++)
		strscpy(data + i * ETH_GSTRING_LEN, xgmib_info[i].name,
			ETH_GSTRING_LEN);
}

/**
 * ppe_mac_get_ethtool_stats() - Get PPE port ethtool statistics
 * @mac: PPE port
 * @data: pointer to statistics data
 *
 * Description: Get the MAC statistics for the PPE port specified
 * by @mac. The statistics are stored in the buffer indicated
 * by @data which used in the ethtool ops.
 */
void ppe_mac_get_ethtool_stats(struct ppe_mac *mac, u64 *data)
{
	int i;

	if (!mac->ppe_dev->data->mac->mib)
		return;

	for (i = 0; i < ARRAY_SIZE(xgmib_info); i++)
		data[i] = ppe_mac_xgmib_get(mac, i);

	/* Merge the GMIB statistics into the XGMIB statistics to show
	 * the total counters for this interface.
	 */
	if (mac->gmib_stats) {
		u64 *gsrc = mac->gmib_stats;

		spin_lock(&mac->gmib_stats_lock);

		ppe_mac_gmib_update(mac);

		data[xgmib_tx_bytes] += gsrc[gmib_tx_bytes];
		data[xgmib_tx_frames] += gsrc[gmib_tx_broadcast];
		data[xgmib_tx_frames] += gsrc[gmib_tx_multicast];
		data[xgmib_tx_frames] += gsrc[gmib_tx_unicast];
		data[xgmib_tx_broadcast_g] += gsrc[gmib_tx_broadcast];
		data[xgmib_tx_multicast_g] += gsrc[gmib_tx_multicast];
		data[xgmib_tx_pkt64] += gsrc[gmib_tx_pkt64];
		data[xgmib_tx_pkt65to127] += gsrc[gmib_tx_pkt65to127];
		data[xgmib_tx_pkt128to255] += gsrc[gmib_tx_pkt128to255];
		data[xgmib_tx_pkt256to511] += gsrc[gmib_tx_pkt256to511];
		data[xgmib_tx_pkt512to1023] += gsrc[gmib_tx_pkt512to1023];
		data[xgmib_tx_pkt1024tomax] += gsrc[gmib_tx_pkt1024to1518];
		data[xgmib_tx_pkt1024tomax] += gsrc[gmib_tx_pkt1519tomax];
		data[xgmib_tx_unicast] += gsrc[gmib_tx_unicast];
		data[xgmib_tx_multicast] += gsrc[gmib_tx_multicast];
		data[xgmib_tx_broadcast] += gsrc[gmib_tx_broadcast];
		data[xgmib_tx_underflow_err] += gsrc[gmib_tx_underrun];
		data[xgmib_tx_bytes_g] += gsrc[gmib_tx_bytes];
		data[xgmib_tx_frames_g] += gsrc[gmib_tx_broadcast];
		data[xgmib_tx_frames_g] += gsrc[gmib_tx_multicast];
		data[xgmib_tx_frames_g] += gsrc[gmib_tx_unicast];
		data[xgmib_tx_pause] += gsrc[gmib_tx_pause];

		data[xgmib_rx_frames] += gsrc[gmib_rx_broadcast];
		data[xgmib_rx_frames] += gsrc[gmib_rx_multicast];
		data[xgmib_rx_frames] += gsrc[gmib_rx_unicast];
		data[xgmib_rx_bytes] += gsrc[gmib_rx_bytes_g];
		data[xgmib_rx_bytes] += gsrc[gmib_rx_bytes_b];
		data[xgmib_rx_bytes_g] += gsrc[gmib_rx_bytes_g];
		data[xgmib_rx_broadcast_g] += gsrc[gmib_rx_broadcast];
		data[xgmib_rx_multicast_g] += gsrc[gmib_rx_multicast];
		data[xgmib_rx_crc_err] += gsrc[gmib_rx_fcserr];
		data[xgmib_rx_crc_err] += gsrc[gmib_rx_frag];
		data[xgmib_rx_frag_err] += gsrc[gmib_rx_frag];
		data[xgmib_rx_pkt64] += gsrc[gmib_rx_pkt64];
		data[xgmib_rx_pkt65to127] += gsrc[gmib_rx_pkt65to127];
		data[xgmib_rx_pkt128to255] += gsrc[gmib_rx_pkt128to255];
		data[xgmib_rx_pkt256to511] += gsrc[gmib_rx_pkt256to511];
		data[xgmib_rx_pkt512to1023] += gsrc[gmib_rx_pkt512to1023];
		data[xgmib_rx_pkt1024tomax] += gsrc[gmib_rx_pkt1024to1518];
		data[xgmib_rx_pkt1024tomax] += gsrc[gmib_rx_pkt1519tomax];
		data[xgmib_rx_unicast_g] += gsrc[gmib_rx_unicast];
		data[xgmib_rx_pause] += gsrc[gmib_rx_pause];

		spin_unlock(&mac->gmib_stats_lock);
	}
}

/**
 * ppe_mac_get_stats64() - Get PPE port statistics
 * @mac: PPE port
 * @s: statistics pointer
 *
 * Description: Get the MAC statistics for the PPE port specified
 * by @mac.
 */
void ppe_mac_get_stats64(struct ppe_mac *mac,
			 struct rtnl_link_stats64 *s)
{
	s->multicast = ppe_mac_xgmib_get(mac, xgmib_rx_multicast_g);

	s->rx_packets = s->multicast;
	s->rx_packets += ppe_mac_xgmib_get(mac, xgmib_rx_unicast_g);
	s->rx_packets += ppe_mac_xgmib_get(mac, xgmib_rx_broadcast_g);

	s->tx_packets = ppe_mac_xgmib_get(mac, xgmib_tx_frames);
	s->rx_bytes = ppe_mac_xgmib_get(mac, xgmib_rx_bytes);
	s->tx_bytes = ppe_mac_xgmib_get(mac, xgmib_tx_bytes);

	s->rx_crc_errors = ppe_mac_xgmib_get(mac, xgmib_rx_crc_err);
	s->rx_fifo_errors = ppe_mac_xgmib_get(mac,
					      xgmib_rx_fifo_overflow);

	s->rx_length_errors = ppe_mac_xgmib_get(mac, xgmib_rx_len_err);
	s->rx_errors = s->rx_crc_errors +
		s->rx_fifo_errors + s->rx_length_errors;
	s->rx_dropped = s->rx_errors;

	s->tx_fifo_errors = ppe_mac_xgmib_get(mac,
					      xgmib_tx_underflow_err);
	s->tx_errors = s->tx_packets -
		ppe_mac_xgmib_get(mac, xgmib_tx_frames_g);

	if (mac->gmib_stats) {
		u64 *gsrc = mac->gmib_stats;
		u64 temp;

		spin_lock(&mac->gmib_stats_lock);

		ppe_mac_gmib_update(mac);

		s->multicast += gsrc[gmib_rx_multicast];
		s->rx_packets += gsrc[gmib_rx_unicast];
		s->rx_packets += gsrc[gmib_rx_broadcast];
		s->rx_packets += gsrc[gmib_rx_multicast];
		s->tx_packets += gsrc[gmib_tx_unicast];
		s->tx_packets += gsrc[gmib_tx_broadcast];
		s->tx_packets += gsrc[gmib_tx_multicast];
		s->rx_bytes += gsrc[gmib_rx_bytes_g];
		s->tx_bytes += gsrc[gmib_tx_bytes];
		temp = gsrc[gmib_rx_fcserr] + gsrc[gmib_rx_frag];
		s->rx_crc_errors += temp;
		temp += gsrc[gmib_rx_alignerr];
		s->rx_errors += temp;
		s->rx_dropped += temp;
		s->tx_fifo_errors += gsrc[gmib_tx_underrun];
		s->tx_errors += gsrc[gmib_tx_underrun];
		s->tx_errors += gsrc[gmib_tx_abortcol];

		spin_unlock(&mac->gmib_stats_lock);
	}
}

/**
 * ppe_mac_lpbk_init - Set up the loopback port.
 * @ppe_dev: PPE device.
 *
 * The loopback port uses the GMAC after the physical ones as an internal drain
 * path. Its enable register has the loopback bits in place of the RX and TX
 * enable bits.
 */
void ppe_mac_lpbk_init(struct ppe_device *ppe_dev)
{
	unsigned int port = ppe_dev->data->loopback_port;

	/* Only some types of PPE have the loopback port. */
	if (!port)
		return;

	regmap_update_bits(ppe_dev->regmap,
			   PPE_PORT_GMAC_ADDR(port) + PPE_LPBK_PPS_CTRL_INC,
			   PPE_LPBK_PPS_THRESHOLD,
			   FIELD_PREP(PPE_LPBK_PPS_THRESHOLD, 21));
	regmap_write(ppe_dev->regmap, PPE_PORT_GMAC_ADDR(port),
		     PPE_LPBK_EN | PPE_LPBK_CRC_STRIP_EN);
	msleep(100);
	ppe_port_txmac_set(ppe_dev, port, true);
}

/**
 * ppe_mac_pcs_mux_init - Set the mux that port 3 shares with the first PCS.
 * @ppe_dev: PPE device.
 *
 * Port 3 shares the channel 4 of the first PCS with the mux, which has to be
 * set before the port comes up.
 */
void ppe_mac_pcs_mux_init(struct ppe_device *ppe_dev)
{
	struct device_node *ports_np;
	int port3_ch = -1;

	ports_np = of_get_child_by_name(ppe_dev->dev->of_node, "ethernet-ports");
	if (!ports_np)
		return;

	for_each_available_child_of_node_scoped(ports_np, port_np) {
		struct of_phandle_args pcs_args;
		u32 port;

		if (of_property_read_u32(port_np, "reg", &port) || port != 3)
			continue;

		if (of_parse_phandle_with_args(port_np, "pcs-handle",
					       "#pcs-cells", 0, &pcs_args))
			continue;

		port3_ch = pcs_args.args[0];
		of_node_put(pcs_args.np);
	}

	of_node_put(ports_np);

	if (ppe_dev->data->mac->mux == PPE_MAC_MUX_IPQ6018 && port3_ch == 4)
		regmap_update_bits(ppe_dev->regmap, PPE_PORT_MUX_CTRL_ADDR,
				   PPE_CPPE_PORT3_PCS_SEL | PPE_CPPE_PCS0_CH4_SEL,
				   FIELD_PREP(PPE_CPPE_PORT3_PCS_SEL,
					      PPE_CPPE_PORT3_PCS0_CH4) |
				   PPE_CPPE_PCS0_CH4_SEL);
}
