/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __EDMAV2_CFG_TX_H__
#define __EDMAV2_CFG_TX_H__

#include <linux/types.h>

struct edmav2;

/* Tx mitigation timer's default value in microseconds. */
#define EDMAV2_TX_MITIGATION_TIMER_DEF	250

/* Tx mitigation packet count default value. */
#define EDMAV2_TX_MITIGATION_PKT_CNT_DEF	16

void edmav2_cfg_tx_rings(struct edmav2 *priv);
int edmav2_cfg_tx_rings_alloc(struct edmav2 *priv);
void edmav2_cfg_tx_rings_cleanup(struct edmav2 *priv);
void edmav2_cfg_tx_disable_interrupts(struct edmav2 *priv);
void edmav2_cfg_tx_enable_interrupts(struct edmav2 *priv);
void edmav2_cfg_tx_napi_enable(struct edmav2 *priv);
void edmav2_cfg_tx_napi_disable(struct edmav2 *priv);
void edmav2_cfg_tx_napi_delete(struct edmav2 *priv);
void edmav2_cfg_tx_napi_add(struct edmav2 *priv);
void edmav2_cfg_tx_ring_mappings(struct edmav2 *priv);
void edmav2_cfg_tx_rings_enable(struct edmav2 *priv);
void edmav2_cfg_tx_rings_disable(struct edmav2 *priv);
#endif
