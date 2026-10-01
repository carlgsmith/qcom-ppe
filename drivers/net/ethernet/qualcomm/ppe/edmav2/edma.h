/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

/* EDMA v2, the IPQ9574 and IPQ5424 generation. A frame has an 8-word
 * descriptor. The rings are shared by all ports: the Tx rings of a port have
 * one ring for each CPU.
 */

#ifndef __EDMAV2_H__
#define __EDMAV2_H__

#include "../edma.h"
#include "rx.h"
#include "tx.h"

/* One clock cycle = 1/(EDMA clock frequency in Mhz) micro seconds.
 *
 * One timer unit is 128 clock cycles.
 *
 * So, therefore the microsecond to timer unit calculation is:
 * Timer unit = time in microseconds / (one clock cycle in microsecond * cycles in 1 timer unit)
 *            = ('x' microsecond * EDMA clock frequency in MHz ('y') / 128).
 *
 */
#define EDMAV2_CYCLE_PER_TIMER_UNIT	128
#define EDMAV2_MICROSEC_TO_TIMER_UNIT(x, y)	((x) * (y) / EDMAV2_CYCLE_PER_TIMER_UNIT)
#define MHZ			1000000UL

/* Number of PPE queue priorities supported per ARM core. */
#define EDMAV2_PRI_MAX_PER_CORE	8

/* The profile of the CPU port in the queue tables of the PPE. */
#define EDMAV2_CPU_PORT_PROFILE_ID	0

/* The port numbers of the physical ports start at 1. */
#define EDMAV2_START_IFNUM   1

#define EDMAV2_DESC_AVAIL_COUNT(head, tail, _max) ({ \
			typeof(_max) (max) = (_max); \
			((((head) - (tail)) + \
			(max)) & ((max) - 1)); })

/**
 * struct edmav2_err_stats - EDMA error stats
 * @edmav2_axi_read_err: AXI read error
 * @edmav2_axi_write_err: AXI write error
 * @edmav2_rxdesc_fifo_full: Rx desc FIFO full error
 * @edmav2_rx_buf_size_err: Rx buffer size too small error
 * @edmav2_tx_sram_full: Tx packet SRAM buffer full error
 * @edmav2_tx_data_len_err: Tx data length error
 * @edmav2_tx_timeout: Tx timeout error
 * @edmav2_txcmpl_buf_full: Tx completion buffer full error
 * @syncp: Synchronization pointer
 */
struct edmav2_err_stats {
	u64 edmav2_axi_read_err;
	u64 edmav2_axi_write_err;
	u64 edmav2_rxdesc_fifo_full;
	u64 edmav2_rx_buf_size_err;
	u64 edmav2_tx_sram_full;
	u64 edmav2_tx_data_len_err;
	u64 edmav2_tx_timeout;
	u64 edmav2_txcmpl_buf_full;
	struct u64_stats_sync syncp;
};

/**
 * struct edmav2_ring_info - EDMA ring data structure.
 * @max_rings: Maximum number of rings
 * @ring_start: Ring start ID
 * @num_rings: Number of rings
 */
struct edmav2_ring_info {
	u32 max_rings;
	u32 ring_start;
	u32 num_rings;
};

/**
 * struct edmav2_hw_info - Data of the SoC for the EDMA.
 * @rxfill: Rx Fill ring information
 * @rx: Rx Desc ring information
 * @tx: Tx Desc ring information
 * @txcmpl: Tx complete ring information
 * @max_ports: Maximum number of ports
 * @napi_budget_rx: Rx NAPI budget
 * @napi_budget_tx: Tx NAPI budget
 * @tso_max: Max segment processing capacity of HW for TSO
 * @idx_mask: Mask for producer, consumer index and ring size
 * @txdesc_fc_grp_id_mask: Mask of the flow control group in the Tx descriptor
 *	ring control register
 * @rxdesc_no_pl_offset: The Rx descriptor ring size register has no payload
 *	offset field
 * @rxfill_size_in_buffer1_reg: The RxFill ring size is in the register of the
 *	first buffer size
 */
struct edmav2_hw_info {
	const struct edmav2_ring_info *rxfill;
	const struct edmav2_ring_info *rx;
	const struct edmav2_ring_info *tx;
	const struct edmav2_ring_info *txcmpl;
	u32 max_ports;
	u32 napi_budget_rx;
	u32 napi_budget_tx;
	u32 tso_max;
	u32 idx_mask;
	u32 txdesc_fc_grp_id_mask;
	bool rxdesc_no_pl_offset;
	bool rxfill_size_in_buffer1_reg;
};

/**
 * struct edmav2_intr_info - EDMA interrupt data structure.
 * @intr_mask_rx: RX interrupt mask
 * @intr_rx: Rx interrupts
 * @intr_mask_txcmpl: Tx completion interrupt mask
 * @intr_txcmpl: Tx completion interrupts
 * @intr_mask_misc: Miscellaneous interrupt mask
 * @intr_misc: Miscellaneous interrupts
 */
struct edmav2_intr_info {
	u32 intr_mask_rx;
	int *intr_rx;
	u32 intr_mask_txcmpl;
	int *intr_txcmpl;
	u32 intr_mask_misc;
	int intr_misc;
};

/**
 * struct edmav2 - EDMA v2 instance.
 * @edma: EDMA
 * @ppe_dev: PPE device
 * @hw_info: EDMA Hardware info
 * @intr_info: EDMA Interrupt info
 * @rxfill_rings: Rx fill Rings, SW is producer
 * @rx_rings: Rx Desc Rings, SW is consumer
 * @tx_rings: Tx Descriptor Ring, SW is producer
 * @txcmpl_rings: Tx complete Ring, SW is consumer
 * @err_stats: Per CPU error statistics
 */
struct edmav2 {
	struct edma *edma;
	struct ppe_device *ppe_dev;
	const struct edmav2_hw_info *hw_info;
	struct edmav2_intr_info intr_info;
	struct edmav2_rxfill_ring *rxfill_rings;
	struct edmav2_rxdesc_ring *rx_rings;
	struct edmav2_txdesc_ring *tx_rings;
	struct edmav2_txcmpl_ring *txcmpl_rings;
	struct edmav2_err_stats __percpu *err_stats;
};

int edmav2_init(struct edma *edma);
void edmav2_fini(struct edma *edma);
void edmav2_open(struct edma *edma);
void edmav2_close(struct edma *edma);
const struct edma_stat_desc *edmav2_stats_layout(struct edma *edma,
						 unsigned int *count);
void edmav2_stats_read(struct edma *edma, u64 *buf);
int edmav2_ringparam_get(struct edma *edma, struct ethtool_ringparam *rp);
#endif /* __EDMAV2_H__ */
