/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __EDMAV1_CFG_TX_H__
#define __EDMAV1_CFG_TX_H__

#include "edma.h"

void edmav1_tx_irq_mask_one(struct edmav1 *priv, u8 ring);
void edmav1_tx_irq_unmask_one(struct edmav1 *priv, u8 ring);
void edmav1_tx_irq_mask_all(struct edmav1 *priv);
void edmav1_tx_irq_unmask_all(struct edmav1 *priv);
int edmav1_tx_ring_alloc(struct edmav1 *priv, struct edmav1_ring *ring, int count,
			 int desc_size);
void edmav1_tx_ring_free(struct edmav1_tx_queue *txq, struct edmav1_ring *ring, int desc_size);
void edmav1_configure_txdesc_ring(struct edmav1_tx_queue *txq);
void edmav1_configure_txcmpl_ring(struct edmav1_tx_queue *txq);
int edmav1_tx_rings_alloc(struct edmav1 *priv);
void edmav1_tx_rings_free_all(struct edmav1 *priv);
void edmav1_tx_rings_drain(struct edmav1 *priv);
void edmav1_configure_tx_rings(struct edmav1 *priv);
void edmav1_tx_rings_disable(struct edmav1 *priv);
void edmav1_tx_rings_enable(struct edmav1 *priv);

#endif /* __EDMAV1_CFG_TX_H__ */
