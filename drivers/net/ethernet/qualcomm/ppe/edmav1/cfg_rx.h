/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __EDMAV1_CFG_RX_H__
#define __EDMAV1_CFG_RX_H__

#include "edma.h"

void edmav1_rxdesc_irq_mask_one(struct edmav1 *priv, u8 ring);
void edmav1_rxdesc_irq_unmask_one(struct edmav1 *priv, u8 ring);
void edmav1_rx_irq_mask_all(struct edmav1 *priv);
void edmav1_rx_irq_unmask_all(struct edmav1 *priv);
int edmav1_rx_ring_alloc(struct edmav1 *priv, struct edmav1_ring *ring, int count,
			 int desc_size);
void edmav1_rx_ring_free(struct edmav1_rx_queue *rxq, struct edmav1_ring *ring, int desc_size);
void edmav1_configure_rxdesc_ring(struct edmav1_rx_queue *rxq);
void edmav1_configure_rxfill_ring(struct edmav1_rx_queue *rxq);
u8 edmav1_rx_page_order(unsigned int frame_size);
u32 edmav1_rx_buffer_size(u8 order);
void edmav1_page_pools_destroy(struct edmav1 *priv);
int edmav1_page_pools_create(struct edmav1 *priv, u8 order);
int edmav1_rx_rings_alloc(struct edmav1 *priv);
void edmav1_rx_rings_drain(struct edmav1 *priv);
void edmav1_configure_rx_rings(struct edmav1 *priv);
void edmav1_rx_rings_disable(struct edmav1 *priv);
void edmav1_rx_rings_enable(struct edmav1 *priv);

#endif /* __EDMAV1_CFG_RX_H__ */
