/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __EDMAV2_CFG_RX_H__
#define __EDMAV2_CFG_RX_H__

#include <linux/types.h>

struct edmav2;

/* Rx flow control X-OFF default value */
#define EDMAV2_RX_FC_XOFF_DEF	32

/* Rx flow control X-ON default value */
#define EDMAV2_RX_FC_XON_DEF	64

/* Rx mitigation timer's default value in microseconds */
#define EDMAV2_RX_MITIGATION_TIMER_DEF	25

/* Rx mitigation packet count's default value */
#define EDMAV2_RX_MITIGATION_PKT_CNT_DEF	16

int edmav2_cfg_rx_rings(struct edmav2 *priv);
int edmav2_cfg_rx_rings_alloc(struct edmav2 *priv);
void edmav2_cfg_rx_ring_mappings(struct edmav2 *priv);
void edmav2_cfg_rx_rings_cleanup(struct edmav2 *priv);
void edmav2_cfg_rx_disable_interrupts(struct edmav2 *priv);
void edmav2_cfg_rx_enable_interrupts(struct edmav2 *priv);
void edmav2_cfg_rx_napi_disable(struct edmav2 *priv);
void edmav2_cfg_rx_napi_enable(struct edmav2 *priv);
void edmav2_cfg_rx_napi_delete(struct edmav2 *priv);
void edmav2_cfg_rx_napi_add(struct edmav2 *priv);
void edmav2_cfg_rx_rings_enable(struct edmav2 *priv);
void edmav2_cfg_rx_rings_disable(struct edmav2 *priv);
void edmav2_cfg_rx_rings_disable_all(struct edmav2 *priv);
int edmav2_cfg_rx_rps_hash_map(struct edmav2 *priv);
#endif
