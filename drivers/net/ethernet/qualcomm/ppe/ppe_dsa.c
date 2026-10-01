// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

/* The PPE as a DSA switch. The ports of the switch core are the user ports
 * and port 0 is the CPU port. The frames of the CPU port reach the host
 * through the Ethernet DMA, and the DSA tag tells the ports apart.
 *
 * Bridges use the VSI of the bridge and no VLAN filtering. The switch does
 * not support VLANs yet.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/if_bridge.h>
#include <linux/if_vlan.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/phylink.h>
#include <linux/regmap.h>
#include <net/dsa.h>

#include "ppe.h"
#include "ppe_config.h"
#include "ppe_dsa.h"
#include "ppe_mac.h"
#include "ppe_regs.h"

#define PPE_DSA_MAX_PORTS	8
#define PPE_DSA_CPU_PORT	0
#define PPE_DSA_MAX_BRIDGES	8
#define PPE_DSA_MAX_FRAME_SIZE	12288
#define PPE_DSA_AGE_UNIT_MS	8000

struct ppe_dsa_bridge_vsi {
	struct net_device *br_dev;
	u32 vsi;
	int refcount;
};

struct ppe_dsa_priv {
	struct dsa_switch ds;
	struct ppe_device *ppe_dev;
	/* Protects port_vsi, port_br_dev, bridges and the FDB and VSI updates
	 * that read an entry before they write it.
	 */
	struct mutex vlan_lock;
	u32 port_vsi[PPE_DSA_MAX_PORTS];
	struct net_device *port_br_dev[PPE_DSA_MAX_PORTS];
	struct ppe_dsa_bridge_vsi bridges[PPE_DSA_MAX_BRIDGES];
};

static struct ppe_dsa_priv *ds_to_priv(struct dsa_switch *ds)
{
	return container_of(ds, struct ppe_dsa_priv, ds);
}

/* The loopback port is an internal port of the switch core. It has no DSA
 * port, and only its gate in the bridge control register is used.
 */
static unsigned int ppe_dsa_lpbk_port(struct ppe_dsa_priv *priv)
{
	return priv->ppe_dev->data->loopback_port;
}

/* The gate of a port in the bridge control register lets the switch fabric
 * feed the port. The loopback port has no link, so a walk over the ports
 * must not close its gate: nothing would open it again.
 */
static void ppe_dsa_bridge_txmac_set(struct ppe_dsa_priv *priv, int port,
				     bool enable)
{
	if (!enable && port == ppe_dsa_lpbk_port(priv))
		return;

	ppe_port_txmac_set(priv->ppe_dev, port, enable);
}

static struct ppe_dsa_bridge_vsi *
bridge_vsi_find(struct ppe_dsa_priv *priv, struct net_device *br_dev)
{
	int i;

	for (i = 0; i < PPE_DSA_MAX_BRIDGES; i++)
		if (priv->bridges[i].br_dev == br_dev)
			return &priv->bridges[i];

	return NULL;
}

static struct ppe_dsa_bridge_vsi *
bridge_vsi_alloc(struct ppe_dsa_priv *priv, struct net_device *br_dev)
{
	int vsi, i;

	vsi = ppe_vsi_alloc(priv->ppe_dev);
	if (vsi < 0)
		return NULL;

	for (i = 0; i < PPE_DSA_MAX_BRIDGES; i++) {
		if (priv->bridges[i].br_dev)
			continue;

		priv->bridges[i].br_dev = br_dev;
		priv->bridges[i].vsi = vsi;
		priv->bridges[i].refcount = 0;

		return &priv->bridges[i];
	}

	ppe_vsi_free(priv->ppe_dev, vsi);

	return NULL;
}

static void bridge_vsi_put(struct ppe_dsa_priv *priv,
			   struct ppe_dsa_bridge_vsi *bvsi)
{
	if (--bvsi->refcount > 0)
		return;

	ppe_vsi_free(priv->ppe_dev, bvsi->vsi);
	bvsi->br_dev = NULL;
	bvsi->vsi = 0;
}

static void bridge_vsi_members_update(struct ppe_dsa_priv *priv,
				      struct ppe_dsa_bridge_vsi *bvsi)
{
	u32 portmask = BIT(PPE_DSA_CPU_PORT);
	int i;

	for (i = 0; i < priv->ds.num_ports; i++)
		if (priv->port_vsi[i] == bvsi->vsi)
			portmask |= BIT(i);

	ppe_vsi_member_set(priv->ppe_dev, bvsi->vsi, portmask);
}

static u32 ppe_dsa_fdb_vsi(struct ppe_dsa_priv *priv, struct dsa_db db)
{
	struct ppe_dsa_bridge_vsi *bvsi;

	if (db.type != DSA_DB_BRIDGE)
		return PPE_VSI_INVALID;

	bvsi = bridge_vsi_find(priv, db.bridge.dev);

	return bvsi ? bvsi->vsi : PPE_VSI_INVALID;
}

static int ppe_dsa_setup(struct dsa_switch *ds)
{
	struct ppe_dsa_priv *priv = ds_to_priv(ds);
	struct ppe_device *ppe_dev = priv->ppe_dev;
	u32 frame_size = ETH_HLEN + ETH_DATA_LEN + 2 * VLAN_HLEN;
	u32 port_mask = BIT(ds->num_ports) - 1;
	int i, ret;

	for (i = 0; i < ds->num_ports; i++)
		priv->port_vsi[i] = PPE_VSI_INVALID;

	ret = regmap_write(ppe_dev->regmap, PPE_FDB_OP_ADDR, 0);
	if (ret)
		return ret;

	for (i = 0; i < ds->num_ports; i++) {
		ret = ppe_port_fabric_setup(ppe_dev, i, frame_size, port_mask);
		if (ret)
			return ret;
	}

	/* VSI 0 is the default VSI. */
	ppe_vsi_reserve(ppe_dev, 0);
	ret = ppe_vsi_set(ppe_dev, 0,
			  dsa_user_ports(ds) | BIT(PPE_DSA_CPU_PORT),
			  BIT(PPE_DSA_CPU_PORT), BIT(PPE_DSA_CPU_PORT),
			  BIT(PPE_DSA_CPU_PORT));
	if (ret)
		return ret;

	for (i = 1; i < ds->num_ports; i++) {
		ret = ppe_user_port_setup(ppe_dev, i, 0);
		if (ret)
			return ret;
	}

	ret = ppe_fdb_flush(ppe_dev);
	if (ret)
		return ret;

	ret = ppe_l2_egress_init(ppe_dev, dsa_user_ports(ds));
	if (ret)
		return ret;

	ret = regmap_set_bits(ppe_dev->regmap, PPE_L2_GLOBAL_CONF_ADDR,
			      PPE_L2_LRN_EN | PPE_L2_AGE_EN);
	if (ret)
		return ret;

	ds->ageing_time_min = PPE_DSA_AGE_UNIT_MS;
	ds->ageing_time_max = min_t(u64, (u64)PPE_DSA_AGE_UNIT_MS *
				    FIELD_MAX(PPE_AGE_TIMER_MASK), U32_MAX);
	ds->assisted_learning_on_cpu_port = true;

	return 0;
}

static int ppe_dsa_set_ageing_time(struct dsa_switch *ds, unsigned int msecs)
{
	struct ppe_dsa_priv *priv = ds_to_priv(ds);

	return regmap_update_bits(priv->ppe_dev->regmap, PPE_AGE_TIMER_ADDR,
				  PPE_AGE_TIMER_MASK,
				  FIELD_PREP(PPE_AGE_TIMER_MASK,
					     msecs / PPE_DSA_AGE_UNIT_MS));
}

static int ppe_dsa_change_mtu(struct dsa_switch *ds, int port, int new_mtu)
{
	struct ppe_dsa_priv *priv = ds_to_priv(ds);

	return ppe_port_mtu_set(priv->ppe_dev, port,
				new_mtu + ETH_HLEN + 2 * VLAN_HLEN);
}

static int ppe_dsa_max_mtu(struct dsa_switch *ds, int port)
{
	return PPE_DSA_MAX_FRAME_SIZE - ETH_HLEN - ETH_FCS_LEN - 2 * VLAN_HLEN;
}

static int ppe_dsa_port_enable(struct dsa_switch *ds, int port,
			       struct phy_device *phy)
{
	struct ppe_dsa_priv *priv = ds_to_priv(ds);

	/* The gate of a user port opens when its MAC is up. DSA enables the
	 * port before it starts phylink, and a gate opened now would point
	 * the fabric at a MAC that is still down. The CPU port has no MAC to
	 * wait for.
	 */
	if (dsa_is_cpu_port(ds, port))
		ppe_dsa_bridge_txmac_set(priv, port, true);

	return 0;
}

static void ppe_dsa_port_disable(struct dsa_switch *ds, int port)
{
	struct ppe_dsa_priv *priv = ds_to_priv(ds);

	ppe_dsa_bridge_txmac_set(priv, port, false);
}

static void ppe_dsa_stp_state_set(struct dsa_switch *ds, int port, u8 state)
{
	struct ppe_dsa_priv *priv = ds_to_priv(ds);
	u32 stp_state;

	switch (state) {
	case BR_STATE_DISABLED:
		stp_state = PPE_STP_DISABLED;
		break;
	case BR_STATE_BLOCKING:
	case BR_STATE_LISTENING:
		stp_state = PPE_STP_BLOCKING;
		break;
	case BR_STATE_LEARNING:
		stp_state = PPE_STP_LEARNING;
		break;
	case BR_STATE_FORWARDING:
	default:
		stp_state = PPE_STP_FORWARDING;
		break;
	}

	regmap_update_bits(priv->ppe_dev->regmap,
			   PPE_CST_STATE_ADDR + port * PPE_CST_STATE_INC,
			   PPE_STP_STATE_MASK, stp_state);
}

static int ppe_dsa_bridge_join(struct dsa_switch *ds, int port,
			       struct dsa_bridge bridge, bool *tx_fwd_offload,
			       struct netlink_ext_ack *extack)
{
	struct ppe_dsa_priv *priv = ds_to_priv(ds);
	struct ppe_dsa_bridge_vsi *bvsi;
	int ret;

	guard(mutex)(&priv->vlan_lock);

	bvsi = bridge_vsi_find(priv, bridge.dev);
	if (!bvsi) {
		bvsi = bridge_vsi_alloc(priv, bridge.dev);
		if (!bvsi)
			return -ENOSPC;
	}

	ret = ppe_port_vsi_set(priv->ppe_dev, port, bvsi->vsi);
	if (ret) {
		if (!bvsi->refcount)
			bridge_vsi_put(priv, bvsi);
		return ret;
	}

	bvsi->refcount++;
	priv->port_vsi[port] = bvsi->vsi;
	priv->port_br_dev[port] = bridge.dev;
	bridge_vsi_members_update(priv, bvsi);

	return 0;
}

static void ppe_dsa_bridge_leave(struct dsa_switch *ds, int port,
				 struct dsa_bridge bridge)
{
	struct ppe_dsa_priv *priv = ds_to_priv(ds);
	struct ppe_dsa_bridge_vsi *bvsi;

	guard(mutex)(&priv->vlan_lock);

	bvsi = bridge_vsi_find(priv, bridge.dev);
	if (!bvsi)
		return;

	priv->port_vsi[port] = PPE_VSI_INVALID;
	priv->port_br_dev[port] = NULL;
	ppe_port_vsi_set(priv->ppe_dev, port, PPE_VSI_INVALID);
	bridge_vsi_members_update(priv, bvsi);
	bridge_vsi_put(priv, bvsi);
}

static int ppe_dsa_fdb_add(struct dsa_switch *ds, int port,
			   const unsigned char *addr, u16 vid,
			   struct dsa_db db)
{
	struct ppe_dsa_priv *priv = ds_to_priv(ds);
	u32 vsi;

	guard(mutex)(&priv->vlan_lock);

	vsi = ppe_dsa_fdb_vsi(priv, db);
	if (vsi == PPE_VSI_INVALID)
		return -EOPNOTSUPP;

	return ppe_fdb_add(priv->ppe_dev, addr, port, vsi);
}

static int ppe_dsa_fdb_del(struct dsa_switch *ds, int port,
			   const unsigned char *addr, u16 vid,
			   struct dsa_db db)
{
	struct ppe_dsa_priv *priv = ds_to_priv(ds);
	u32 vsi;

	guard(mutex)(&priv->vlan_lock);

	vsi = ppe_dsa_fdb_vsi(priv, db);
	if (vsi == PPE_VSI_INVALID)
		return -EOPNOTSUPP;

	return ppe_fdb_del(priv->ppe_dev, addr, port, vsi);
}

static int ppe_dsa_fdb_dump(struct dsa_switch *ds, int port,
			    dsa_fdb_dump_cb_t *cb, void *data)
{
	struct ppe_dsa_priv *priv = ds_to_priv(ds);
	unsigned char addr[ETH_ALEN];
	bool is_static;
	int fdb_port;
	u32 i, vsi;
	int ret;

	guard(mutex)(&priv->vlan_lock);

	for (i = 0; i < PPE_FDB_TBL_NUM; i++) {
		if (ppe_fdb_read_entry(priv->ppe_dev, i, addr, &vsi, &fdb_port,
				       &is_static))
			continue;

		if (fdb_port != port)
			continue;

		/* There is no VLAN filtering, so every entry has vid 0. */
		ret = cb(addr, 0, is_static, data);
		if (ret)
			return ret;
	}

	return 0;
}

static int ppe_dsa_mdb_add(struct dsa_switch *ds, int port,
			   const struct switchdev_obj_port_mdb *mdb,
			   struct dsa_db db)
{
	struct ppe_dsa_priv *priv = ds_to_priv(ds);
	u32 portmap, vsi;
	int ret;

	guard(mutex)(&priv->vlan_lock);

	vsi = ppe_dsa_fdb_vsi(priv, db);
	if (vsi == PPE_VSI_INVALID)
		return -EOPNOTSUPP;

	ret = ppe_fdb_mcast_lookup(priv->ppe_dev, mdb->addr, vsi, &portmap);
	if (ret)
		portmap = BIT(PPE_DSA_CPU_PORT);

	portmap |= BIT(port);

	return ppe_fdb_mcast_add(priv->ppe_dev, mdb->addr, portmap, vsi);
}

static int ppe_dsa_mdb_del(struct dsa_switch *ds, int port,
			   const struct switchdev_obj_port_mdb *mdb,
			   struct dsa_db db)
{
	struct ppe_dsa_priv *priv = ds_to_priv(ds);
	u32 portmap, vsi;
	int ret;

	guard(mutex)(&priv->vlan_lock);

	vsi = ppe_dsa_fdb_vsi(priv, db);
	if (vsi == PPE_VSI_INVALID)
		return -EOPNOTSUPP;

	ret = ppe_fdb_mcast_lookup(priv->ppe_dev, mdb->addr, vsi, &portmap);

	/* The bridge deletes the multicast entries of a port on every leave,
	 * STP transition and membership expiry, and does not track which of
	 * them the switch programmed. A delete then arrives for an entry that
	 * was never added or that an earlier delete emptied, and the state
	 * that is asked for already holds.
	 */
	if (ret == -ENOENT)
		return 0;
	if (ret)
		return ret;

	if (!(portmap & BIT(port)))
		return 0;

	portmap &= ~BIT(port);

	if (!portmap || portmap == BIT(PPE_DSA_CPU_PORT))
		return ppe_fdb_mcast_del(priv->ppe_dev, mdb->addr, 0, vsi);

	return ppe_fdb_mcast_add(priv->ppe_dev, mdb->addr, portmap, vsi);
}

static enum dsa_tag_protocol
ppe_dsa_get_tag_protocol(struct dsa_switch *ds, int port,
			 enum dsa_tag_protocol mprot)
{
	return DSA_TAG_PROTO_DSA;
}

static void ppe_dsa_phylink_get_caps(struct dsa_switch *ds, int port,
				     struct phylink_config *config)
{
	switch (port) {
	case 0:
		config->mac_capabilities =
			MAC_1000FD | MAC_SYM_PAUSE | MAC_ASYM_PAUSE;

		__set_bit(PHY_INTERFACE_MODE_INTERNAL,
			  config->supported_interfaces);
		break;
	case 1 ... 4:
		config->mac_capabilities =
			MAC_1000FD | MAC_100FD | MAC_10FD |
			MAC_SYM_PAUSE | MAC_ASYM_PAUSE;

		__set_bit(PHY_INTERFACE_MODE_QSGMII,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_PSGMII,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_SGMII,
			  config->supported_interfaces);
		break;
	case 5 ... 6:
		config->mac_capabilities =
			MAC_10000FD | MAC_5000FD | MAC_2500FD |
			MAC_1000FD | MAC_100FD | MAC_10FD |
			MAC_SYM_PAUSE | MAC_ASYM_PAUSE;

		__set_bit(PHY_INTERFACE_MODE_PSGMII,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_SGMII,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_1000BASEX,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_2500BASEX,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_USXGMII,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_10GBASER,
			  config->supported_interfaces);
		break;
	}
}

/* Ports declare their PCS via a "pcs-handle"/"#pcs-cells" phandle in the
 * devicetree. Port 0 has none, since it's the internal CPU port and needs
 * no PCS at all.
 */
static struct phylink_pcs *ppe_dsa_mac_select_pcs(struct phylink_config *config,
						  phy_interface_t interface)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct phylink_pcs *pcs = ppe_mac_pcs_get(dp->dn);

	if (PTR_ERR(pcs) == -ENODEV)
		return NULL;

	return pcs;
}

static struct ppe_mac *ppe_dsa_config_mac(struct phylink_config *config)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);

	return ppe_mac_get(ds_to_priv(dp->ds)->ppe_dev, dp->index);
}

static int ppe_dsa_mac_prepare(struct phylink_config *config, unsigned int mode,
			       phy_interface_t interface)
{
	struct ppe_mac *mac = ppe_dsa_config_mac(config);

	return mac ? ppe_mac_prepare(mac, mode, interface) : 0;
}

static void ppe_dsa_mac_config(struct phylink_config *config,
			       unsigned int mode,
			       const struct phylink_link_state *state)
{
	struct ppe_mac *mac = ppe_dsa_config_mac(config);

	if (mac)
		ppe_mac_config(mac, mode, state->interface);
}

static void ppe_dsa_mac_link_down(struct phylink_config *config,
				  unsigned int mode,
				  phy_interface_t interface)
{
	struct ppe_mac *mac = ppe_dsa_config_mac(config);

	/* The CPU port has no MAC, and its gate stays open. */
	if (mac)
		ppe_mac_link_down(mac, mode, interface);
}

static void ppe_dsa_mac_link_up(struct phylink_config *config,
				struct phy_device *phydev,
				unsigned int mode,
				phy_interface_t interface,
				int speed, int duplex,
				bool tx_pause, bool rx_pause)
{
	struct ppe_mac *mac = ppe_dsa_config_mac(config);

	if (mac)
		ppe_mac_link_up(mac, mode, interface, speed, duplex,
				tx_pause, rx_pause);
}

static const struct phylink_mac_ops ppe_dsa_phylink_mac_ops = {
	.mac_select_pcs	= ppe_dsa_mac_select_pcs,
	.mac_prepare	= ppe_dsa_mac_prepare,
	.mac_config	= ppe_dsa_mac_config,
	.mac_link_down	= ppe_dsa_mac_link_down,
	.mac_link_up	= ppe_dsa_mac_link_up,
};

/* The MIB counters are those of the MAC of the port. The CPU port has no MAC,
 * and a SoC whose MACs have no counters shows none.
 */
static struct ppe_mac *ppe_dsa_mib_mac(struct dsa_switch *ds, int port)
{
	struct ppe_device *ppe_dev = ds_to_priv(ds)->ppe_dev;

	if (!ppe_dev->data->mac->mib)
		return NULL;

	return ppe_mac_get(ppe_dev, port);
}

static int ppe_dsa_get_sset_count(struct dsa_switch *ds, int port, int sset)
{
	struct ppe_mac *mac = ppe_dsa_mib_mac(ds, port);

	return mac ? ppe_mac_get_sset_count(mac, sset) : 0;
}

static void ppe_dsa_get_strings(struct dsa_switch *ds, int port, u32 stringset,
				u8 *data)
{
	struct ppe_mac *mac = ppe_dsa_mib_mac(ds, port);

	if (mac)
		ppe_mac_get_strings(mac, stringset, data);
}

static void ppe_dsa_get_ethtool_stats(struct dsa_switch *ds, int port,
				      u64 *data)
{
	struct ppe_mac *mac = ppe_dsa_mib_mac(ds, port);

	if (mac)
		ppe_mac_get_ethtool_stats(mac, data);
}

static void ppe_dsa_get_stats64(struct dsa_switch *ds, int port,
				struct rtnl_link_stats64 *s)
{
	struct ppe_mac *mac = ppe_dsa_mib_mac(ds, port);

	if (mac)
		ppe_mac_get_stats64(mac, s);
}

static const struct dsa_switch_ops ppe_dsa_ops = {
	.get_tag_protocol	= ppe_dsa_get_tag_protocol,
	.phylink_get_caps	= ppe_dsa_phylink_get_caps,
	.setup			= ppe_dsa_setup,
	.set_ageing_time	= ppe_dsa_set_ageing_time,
	.port_change_mtu	= ppe_dsa_change_mtu,
	.port_max_mtu		= ppe_dsa_max_mtu,
	.port_enable		= ppe_dsa_port_enable,
	.port_disable		= ppe_dsa_port_disable,
	.port_stp_state_set	= ppe_dsa_stp_state_set,
	.port_bridge_join	= ppe_dsa_bridge_join,
	.port_bridge_leave	= ppe_dsa_bridge_leave,
	.port_fdb_add		= ppe_dsa_fdb_add,
	.port_fdb_del		= ppe_dsa_fdb_del,
	.port_fdb_dump		= ppe_dsa_fdb_dump,
	.port_mdb_add		= ppe_dsa_mdb_add,
	.port_mdb_del		= ppe_dsa_mdb_del,
	.get_sset_count		= ppe_dsa_get_sset_count,
	.get_strings		= ppe_dsa_get_strings,
	.get_ethtool_stats	= ppe_dsa_get_ethtool_stats,
	.get_stats64		= ppe_dsa_get_stats64,
};

/* Send the spanning tree and the slow protocol frames of the external ports
 * to the CPU. The frames that the CPU sends still reach the wire. Without
 * the second entry the PPE keeps the LACP frames from the CPU and a bond
 * never sees its partner.
 */
static void ppe_dsa_ctrlpkt_init(struct ppe_dsa_priv *priv)
{
	struct regmap *regmap = priv->ppe_dev->regmap;
	const struct ppe_regs *regs = ppe_regs(priv->ppe_dev);
	u32 ports;

	ports = GENMASK(priv->ppe_dev->num_ports - 1, 0) &
		~(BIT(PPE_DSA_CPU_PORT) | BIT(ppe_dsa_lpbk_port(priv)));

	/* RFDB entry 31 is 01:80:c2:00:00:00 and entry 30 is
	 * 01:80:c2:00:00:02.
	 */
	regmap_write(regmap, regs->rfdb_tbl_addr + 31 * PPE_RFDB_TBL_INC,
		     0xc2000000);
	regmap_write(regmap, regs->rfdb_tbl_addr + 31 * PPE_RFDB_TBL_INC + 4,
		     0x00010180);
	regmap_write(regmap, regs->rfdb_tbl_addr + 30 * PPE_RFDB_TBL_INC,
		     0xc2000002);
	regmap_write(regmap, regs->rfdb_tbl_addr + 30 * PPE_RFDB_TBL_INC + 4,
		     0x00010180);

	/* The rule matches the RFDB entries 30 and 31, bypasses the spanning
	 * tree state and redirects to the CPU.
	 */
	regmap_write(regmap, regs->app_ctrl_addr, 0x00000003);
	regmap_write(regmap, regs->app_ctrl_addr + 4, 0x00000003);
	regmap_write(regmap, regs->app_ctrl_addr + 8,
		     PPE_APP_CTRL_PORT_BITMAP_EN |
		     FIELD_PREP(PPE_APP_CTRL_PORT_BITMAP, ports) |
		     PPE_APP_CTRL_STP_BYPASS |
		     FIELD_PREP(PPE_APP_CTRL_CMD, PPE_APP_CTRL_REDIRECT_CPU));
}

/**
 * ppe_dsa_switch_init - Register the PPE as a DSA switch.
 * @ppe_dev: PPE device.
 *
 * dsa_register_switch() finds the conduit of the CPU port from the
 * "ethernet" property of the port and defers until the conduit exists.
 *
 * Return: 0 on success, negative error code on failure.
 */
int ppe_dsa_switch_init(struct ppe_device *ppe_dev)
{
	struct device *dev = ppe_dev->dev;
	struct ppe_dsa_priv *priv;
	struct dsa_switch *ds;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->ppe_dev = ppe_dev;
	mutex_init(&priv->vlan_lock);

	ds = &priv->ds;
	ds->dev = dev;
	ds->num_ports = ppe_dev->num_ports;
	ds->ops = &ppe_dsa_ops;
	ds->priv = priv;
	ds->phylink_mac_ops = &ppe_dsa_phylink_mac_ops;

	ppe_mac_pcs_mux_init(ppe_dev);
	ppe_mac_lpbk_init(ppe_dev);
	ppe_dsa_ctrlpkt_init(priv);

	ret = dsa_register_switch(ds);
	if (ret)
		return ret;

	ppe_dev->ds = ds;

	return 0;
}

void ppe_dsa_switch_deinit(struct ppe_device *ppe_dev)
{
	if (!ppe_dev->ds)
		return;

	dsa_unregister_switch(ppe_dev->ds);
	ppe_dev->ds = NULL;
}
