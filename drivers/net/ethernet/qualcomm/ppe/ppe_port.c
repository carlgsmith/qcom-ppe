// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

/* The PPE with a netdev for each port. A port node without an "ethernet"
 * property is a netdev, which sends and receives its frames through the
 * Ethernet DMA. The MAC of the port is handled by ppe_mac.c, and the PCS
 * is the one of the "pcs-handle" property.
 */

#include <linux/clk.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/if_vlan.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/phylink.h>
#include <linux/rtnetlink.h>

#include "ppe.h"
#include "ppe_config.h"
#include "ppe_mac.h"
#include "ppe_port.h"

/**
 * struct ppe_port - Private data of the netdev of a port.
 * @ppe_dev: PPE device
 * @mac: MAC of the port
 * @netdev: Netdev of the port
 * @port: Port number
 * @interface: Interface mode of the port
 * @phylink: Phylink instance
 * @config: Phylink configuration
 * @pcs: PCS of the port
 */
struct ppe_port {
	struct ppe_device *ppe_dev;
	struct ppe_mac *mac;
	struct net_device *netdev;
	unsigned int port;
	phy_interface_t interface;
	struct phylink *phylink;
	struct phylink_config config;
	struct phylink_pcs *pcs;
};

static const phy_interface_t ppe_port_interfaces[] = {
	PHY_INTERFACE_MODE_SGMII,
	PHY_INTERFACE_MODE_QSGMII,
	PHY_INTERFACE_MODE_PSGMII,
	PHY_INTERFACE_MODE_1000BASEX,
	PHY_INTERFACE_MODE_2500BASEX,
	PHY_INTERFACE_MODE_USXGMII,
	PHY_INTERFACE_MODE_10GBASER,
	PHY_INTERFACE_MODE_10G_QXGMII,
};

static struct ppe_port *ppe_port_from_config(struct phylink_config *config)
{
	return container_of(config, struct ppe_port, config);
}

static struct phylink_pcs *ppe_port_mac_select_pcs(struct phylink_config *config,
						   phy_interface_t interface)
{
	return ppe_port_from_config(config)->pcs;
}

static int ppe_port_mac_prepare(struct phylink_config *config, unsigned int mode,
				phy_interface_t interface)
{
	return ppe_mac_prepare(ppe_port_from_config(config)->mac, mode, interface);
}

static void ppe_port_mac_config(struct phylink_config *config, unsigned int mode,
				const struct phylink_link_state *state)
{
	ppe_mac_config(ppe_port_from_config(config)->mac, mode, state->interface);
}

static void ppe_port_mac_link_up(struct phylink_config *config,
				 struct phy_device *phy, unsigned int mode,
				 phy_interface_t interface, int speed,
				 int duplex, bool tx_pause, bool rx_pause)
{
	ppe_mac_link_up(ppe_port_from_config(config)->mac, mode, interface,
			speed, duplex, tx_pause, rx_pause);
}

static void ppe_port_mac_link_down(struct phylink_config *config,
				   unsigned int mode, phy_interface_t interface)
{
	ppe_mac_link_down(ppe_port_from_config(config)->mac, mode, interface);
}

static const struct phylink_mac_ops ppe_port_phylink_ops = {
	.mac_select_pcs	= ppe_port_mac_select_pcs,
	.mac_prepare	= ppe_port_mac_prepare,
	.mac_config	= ppe_port_mac_config,
	.mac_link_up	= ppe_port_mac_link_up,
	.mac_link_down	= ppe_port_mac_link_down,
};

static int ppe_port_phylink_setup(struct ppe_port *port, struct device_node *np)
{
	struct device *dev = port->ppe_dev->dev;
	int i, ret;

	port->pcs = ppe_mac_pcs_get(np);
	if (IS_ERR(port->pcs)) {
		ret = PTR_ERR(port->pcs);
		port->pcs = NULL;
		return dev_err_probe(dev, ret, "port %u has no PCS\n", port->port);
	}

	port->config.dev = &port->netdev->dev;
	port->config.type = PHYLINK_NETDEV;
	port->config.mac_capabilities = MAC_ASYM_PAUSE | MAC_SYM_PAUSE |
					MAC_10 | MAC_100 | MAC_1000 |
					MAC_2500FD | MAC_5000FD | MAC_10000FD;

	for (i = 0; i < ARRAY_SIZE(ppe_port_interfaces); i++)
		__set_bit(ppe_port_interfaces[i], port->config.supported_interfaces);

	port->phylink = phylink_create(&port->config, of_fwnode_handle(np),
				       port->interface, &ppe_port_phylink_ops);
	if (IS_ERR(port->phylink)) {
		ret = PTR_ERR(port->phylink);
		port->phylink = NULL;
		goto err_pcs;
	}

	ret = phylink_of_phy_connect(port->phylink, np, 0);
	if (ret)
		goto err_phylink;

	return 0;

err_phylink:
	phylink_destroy(port->phylink);
	port->phylink = NULL;
err_pcs:
	port->pcs = NULL;

	return dev_err_probe(dev, ret, "port %u phylink setup failed\n", port->port);
}

static void ppe_port_phylink_destroy(struct ppe_port *port)
{
	rtnl_lock();
	phylink_disconnect_phy(port->phylink);
	rtnl_unlock();
	phylink_destroy(port->phylink);
}

static int ppe_port_open(struct net_device *netdev)
{
	struct ppe_port *port = netdev_priv(netdev);
	int ret;

	ret = edma_open(port->ppe_dev->edma);
	if (ret)
		return ret;

	phylink_start(port->phylink);
	netif_tx_start_all_queues(netdev);

	return 0;
}

static int ppe_port_stop(struct net_device *netdev)
{
	struct ppe_port *port = netdev_priv(netdev);

	netif_tx_disable(netdev);
	phylink_stop(port->phylink);
	edma_close(port->ppe_dev->edma);

	return 0;
}

static netdev_tx_t ppe_port_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	struct ppe_port *port = netdev_priv(netdev);

	return edma_xmit(port->ppe_dev->edma, skb, port->port,
			 skb_get_queue_mapping(skb));
}

/* The core that transmits picks the queue. The EDMA has a Tx ring for each
 * core of each port.
 */
static u16 ppe_port_select_queue(struct net_device *netdev, struct sk_buff *skb,
				 struct net_device *sb_dev)
{
	return smp_processor_id() % netdev->real_num_tx_queues;
}

/* The receive buffers are sized for the largest frame of the ports. */
static unsigned int ppe_port_frame_size(int mtu)
{
	return mtu + ETH_HLEN + 2 * VLAN_HLEN;
}

static unsigned int ppe_port_largest_frame(struct ppe_device *ppe_dev,
					   struct net_device *netdev, int new_mtu)
{
	unsigned int largest = ppe_port_frame_size(new_mtu);
	unsigned int i;

	for (i = 0; i < PPE_MAC_MAX_PORTS; i++) {
		struct net_device *other = ppe_dev->port_netdev[i];

		if (other && other != netdev)
			largest = max(largest, ppe_port_frame_size(other->mtu));
	}

	return largest;
}

static int ppe_port_change_mtu(struct net_device *netdev, int new_mtu)
{
	struct ppe_port *port = netdev_priv(netdev);
	struct ppe_device *ppe_dev = port->ppe_dev;
	unsigned int i, frame;
	int ret;

	ret = ppe_port_mtu_set(ppe_dev, port->port, ppe_port_frame_size(new_mtu));
	if (ret)
		return ret;

	/* The queues of all ports are stopped while the buffers change. */
	frame = ppe_port_largest_frame(ppe_dev, netdev, new_mtu);
	for (i = 0; i < PPE_MAC_MAX_PORTS; i++)
		if (ppe_dev->port_netdev[i])
			netif_tx_disable(ppe_dev->port_netdev[i]);

	edma_pause(ppe_dev->edma);
	ret = edma_set_max_frame(ppe_dev->edma, frame);
	edma_resume(ppe_dev->edma);

	for (i = 0; i < PPE_MAC_MAX_PORTS; i++) {
		struct net_device *other = ppe_dev->port_netdev[i];

		if (other && netif_running(other))
			netif_tx_start_all_queues(other);
	}

	if (!ret)
		WRITE_ONCE(netdev->mtu, new_mtu);

	return ret;
}

static int ppe_port_set_mac_address(struct net_device *netdev, void *addr)
{
	struct ppe_port *port = netdev_priv(netdev);
	struct sockaddr *sa = addr;
	int ret;

	ret = eth_prepare_mac_addr_change(netdev, addr);
	if (ret)
		return ret;

	ret = ppe_mac_set_address(port->mac, sa->sa_data);
	if (ret)
		return ret;

	eth_commit_mac_addr_change(netdev, addr);

	return 0;
}

static int ppe_port_ioctl(struct net_device *netdev, struct ifreq *ifr, int cmd)
{
	struct ppe_port *port = netdev_priv(netdev);

	return phylink_mii_ioctl(port->phylink, ifr, cmd);
}

/* The MIB counters of the MAC count the frames on the wire. A SoC whose MACs
 * have no counters uses the counters that the EDMA keeps for the netdev.
 */
static void ppe_port_get_stats64(struct net_device *netdev,
				 struct rtnl_link_stats64 *stats)
{
	struct ppe_port *port = netdev_priv(netdev);

	if (port->ppe_dev->data->mac->mib)
		ppe_mac_get_stats64(port->mac, stats);
	else
		dev_get_tstats64(netdev, stats);
}

static const struct net_device_ops ppe_port_netdev_ops = {
	.ndo_open		= ppe_port_open,
	.ndo_stop		= ppe_port_stop,
	.ndo_start_xmit		= ppe_port_xmit,
	.ndo_select_queue	= ppe_port_select_queue,
	.ndo_change_mtu		= ppe_port_change_mtu,
	.ndo_set_mac_address	= ppe_port_set_mac_address,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_eth_ioctl		= ppe_port_ioctl,
	.ndo_get_stats64	= ppe_port_get_stats64,
};

static void ppe_port_get_drvinfo(struct net_device *netdev,
				 struct ethtool_drvinfo *info)
{
	strscpy(info->driver, "qcom-ppe", sizeof(info->driver));
	strscpy(info->bus_info, dev_name(netdev->dev.parent),
		sizeof(info->bus_info));
}

/* The statistics are those of the MAC, followed by those of the EDMA. */
static int ppe_port_get_sset_count(struct net_device *netdev, int sset)
{
	struct ppe_port *port = netdev_priv(netdev);
	unsigned int count;

	if (sset != ETH_SS_STATS)
		return -EOPNOTSUPP;

	edma_stats_layout(port->ppe_dev->edma, &count);

	return ppe_mac_get_sset_count(port->mac, sset) + count;
}

static void ppe_port_get_strings(struct net_device *netdev, u32 sset, u8 *data)
{
	struct ppe_port *port = netdev_priv(netdev);
	const struct edma_stat_desc *descs;
	unsigned int count, i;

	if (sset != ETH_SS_STATS)
		return;

	ppe_mac_get_strings(port->mac, sset, data);
	data += ppe_mac_get_sset_count(port->mac, sset) * ETH_GSTRING_LEN;

	descs = edma_stats_layout(port->ppe_dev->edma, &count);
	for (i = 0; i < count; i++)
		ethtool_puts(&data, descs[i].name);
}

static void ppe_port_get_stats(struct net_device *netdev,
			       struct ethtool_stats *stats, u64 *data)
{
	struct ppe_port *port = netdev_priv(netdev);

	ppe_mac_get_ethtool_stats(port->mac, data);
	edma_stats_read(port->ppe_dev->edma,
			data + ppe_mac_get_sset_count(port->mac, ETH_SS_STATS));
}

static int ppe_port_get_ksettings(struct net_device *netdev,
				  struct ethtool_link_ksettings *cmd)
{
	struct ppe_port *port = netdev_priv(netdev);

	return phylink_ethtool_ksettings_get(port->phylink, cmd);
}

static int ppe_port_set_ksettings(struct net_device *netdev,
				  const struct ethtool_link_ksettings *cmd)
{
	struct ppe_port *port = netdev_priv(netdev);

	return phylink_ethtool_ksettings_set(port->phylink, cmd);
}

static void ppe_port_get_pauseparam(struct net_device *netdev,
				    struct ethtool_pauseparam *pause)
{
	struct ppe_port *port = netdev_priv(netdev);

	phylink_ethtool_get_pauseparam(port->phylink, pause);
}

static int ppe_port_set_pauseparam(struct net_device *netdev,
				   struct ethtool_pauseparam *pause)
{
	struct ppe_port *port = netdev_priv(netdev);

	return phylink_ethtool_set_pauseparam(port->phylink, pause);
}

static int ppe_port_nway_reset(struct net_device *netdev)
{
	struct ppe_port *port = netdev_priv(netdev);

	return phylink_ethtool_nway_reset(port->phylink);
}

static void ppe_port_get_ringparam(struct net_device *netdev,
				   struct ethtool_ringparam *ring,
				   struct kernel_ethtool_ringparam *kernel_ring,
				   struct netlink_ext_ack *extack)
{
	struct ppe_port *port = netdev_priv(netdev);

	edma_ringparam_get(port->ppe_dev->edma, ring);
}

static const struct ethtool_ops ppe_port_ethtool_ops = {
	.get_drvinfo		= ppe_port_get_drvinfo,
	.get_link		= ethtool_op_get_link,
	.get_sset_count		= ppe_port_get_sset_count,
	.get_strings		= ppe_port_get_strings,
	.get_ethtool_stats	= ppe_port_get_stats,
	.get_link_ksettings	= ppe_port_get_ksettings,
	.set_link_ksettings	= ppe_port_set_ksettings,
	.get_pauseparam		= ppe_port_get_pauseparam,
	.set_pauseparam		= ppe_port_set_pauseparam,
	.nway_reset		= ppe_port_nway_reset,
	.get_ringparam		= ppe_port_get_ringparam,
};

static int ppe_port_create(struct ppe_device *ppe_dev, struct device_node *np,
			   unsigned int port_id)
{
	const struct edma_caps *caps = edma_caps_get(ppe_dev->edma);
	struct device *dev = ppe_dev->dev;
	struct net_device *netdev;
	struct ppe_port *port;
	const char *name;
	int assign_type;
	int ret;

	if (of_property_read_string(np, "label", &name)) {
		name = "eth%d";
		assign_type = NET_NAME_ENUM;
	} else {
		assign_type = NET_NAME_PREDICTABLE;
	}

	netdev = alloc_netdev_mqs(sizeof(*port), name, assign_type, ether_setup,
				  caps->tx_queues, caps->rx_queues);
	if (!netdev)
		return -ENOMEM;

	port = netdev_priv(netdev);
	port->ppe_dev = ppe_dev;
	port->mac = ppe_mac_get(ppe_dev, port_id);
	port->netdev = netdev;
	port->port = port_id;

	ret = of_get_phy_mode(np, &port->interface);
	if (ret) {
		dev_err(dev, "port %u has no phy-mode\n", port_id);
		goto err_free;
	}

	SET_NETDEV_DEV(netdev, dev);
	netdev->dev.of_node = np;
	netdev->netdev_ops = &ppe_port_netdev_ops;
	netdev->ethtool_ops = &ppe_port_ethtool_ops;
	netdev->hw_features = caps->features;
	netdev->features = NETIF_F_GRO | caps->features;
	netdev->vlan_features = caps->features;
	netdev->pcpu_stat_type = NETDEV_PCPU_STAT_TSTATS;
	netdev->watchdog_timeo = 5 * HZ;
	netdev->priv_flags |= IFF_LIVE_ADDR_CHANGE;
	netdev->max_mtu = caps->max_mtu;
	netdev->needed_headroom = caps->needed_headroom;

	ret = of_get_ethdev_address(np, netdev);
	if (ret == -EPROBE_DEFER)
		goto err_free;
	if (ret)
		eth_hw_addr_random(netdev);

	ret = ppe_mac_set_address(port->mac, netdev->dev_addr);
	if (ret)
		goto err_free;

	ret = ppe_port_phylink_setup(port, np);
	if (ret)
		goto err_free;

	ret = edma_register_netdev(ppe_dev->edma, port_id, netdev);
	if (ret)
		goto err_phylink;

	ret = register_netdev(netdev);
	if (ret)
		goto err_edma;

	ppe_dev->port_netdev[port_id] = netdev;

	return 0;

err_edma:
	edma_unregister_netdev(ppe_dev->edma, port_id);
err_phylink:
	ppe_port_phylink_destroy(port);
err_free:
	free_netdev(netdev);

	return ret;
}

static void ppe_port_destroy(struct ppe_device *ppe_dev, unsigned int port_id)
{
	struct net_device *netdev = ppe_dev->port_netdev[port_id];
	struct ppe_port *port = netdev_priv(netdev);

	unregister_netdev(netdev);
	edma_unregister_netdev(ppe_dev->edma, port_id);
	ppe_port_phylink_destroy(port);
	free_netdev(netdev);
	ppe_dev->port_netdev[port_id] = NULL;
}

/**
 * ppe_port_deinit - Remove the netdevs of the ports.
 * @ppe_dev: PPE device.
 */
void ppe_port_deinit(struct ppe_device *ppe_dev)
{
	unsigned int i;

	for (i = 0; i < PPE_MAC_MAX_PORTS; i++)
		if (ppe_dev->port_netdev[i])
			ppe_port_destroy(ppe_dev, i);
}

static bool ppe_port_has_netdev(struct ppe_device *ppe_dev,
				struct device_node *port_np, u32 *port_id)
{
	return !of_property_read_u32(port_np, "reg", port_id) &&
	       *port_id < PPE_MAC_MAX_PORTS && *port_id != PPE_CPU_PORT &&
	       ppe_mac_get(ppe_dev, *port_id);
}

/**
 * ppe_port_init - Create a netdev for each port.
 * @ppe_dev: PPE device.
 *
 * A port has a netdev when its node is available and it has a MAC.
 *
 * Return: 0 on success, negative error code on failure.
 */
int ppe_port_init(struct ppe_device *ppe_dev)
{
	struct device_node *ports_np;
	u32 port_id;
	int ret = 0;

	if (!ppe_dev->edma || !ppe_dev->data->mac)
		return -EOPNOTSUPP;

	ports_np = of_get_child_by_name(ppe_dev->dev->of_node, "ethernet-ports");
	if (!ports_np)
		return -ENODEV;

	for_each_available_child_of_node_scoped(ports_np, port_np) {
		if (!ppe_port_has_netdev(ppe_dev, port_np, &port_id))
			continue;

		ret = ppe_port_create(ppe_dev, port_np, port_id);
		if (ret)
			break;
	}

	of_node_put(ports_np);
	if (ret)
		ppe_port_deinit(ppe_dev);

	return ret;
}
