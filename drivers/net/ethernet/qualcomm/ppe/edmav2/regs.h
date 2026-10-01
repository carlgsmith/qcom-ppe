/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

/* Registers of the EDMA v2. The offsets are relative to EDMA_BASE_OFFSET. */

#ifndef __EDMAV2_REGS_H__
#define __EDMAV2_REGS_H__

/* EDMA register offsets */
#define EDMAV2_REG_MAS_CTRL_ADDR			0x0
#define EDMAV2_REG_PORT_CTRL_ADDR			0x4
#define EDMAV2_REG_VLAN_CTRL_ADDR			0x8
#define EDMAV2_REG_RXDESC2FILL_MAP_0_ADDR		0x14
#define EDMAV2_REG_RXDESC2FILL_MAP_1_ADDR		0x18
#define EDMAV2_REG_RXDESC2FILL_MAP_2_ADDR		0x1c
#define EDMAV2_REG_TXQ_CTRL_ADDR			0x20
#define EDMAV2_REG_TXQ_CTRL_2_ADDR		0x24
#define EDMAV2_REG_TXQ_FC_0_ADDR			0x28
#define EDMAV2_REG_TXQ_FC_1_ADDR			0x30
#define EDMAV2_REG_TXQ_FC_2_ADDR			0x34
#define EDMAV2_REG_TXQ_FC_3_ADDR			0x38
#define EDMAV2_REG_RXQ_CTRL_ADDR			0x3c
#define EDMAV2_REG_MISC_ERR_QID_ADDR		0x40
#define EDMAV2_REG_RXQ_FC_THRE_ADDR		0x44
#define EDMAV2_REG_DMAR_CTRL_ADDR			0x48
#define EDMAV2_REG_AXIR_CTRL_ADDR			0x4c
#define EDMAV2_REG_AXIW_CTRL_ADDR			0x50
#define EDMAV2_REG_MIN_MSS_ADDR			0x54
#define EDMAV2_REG_LOOPBACK_CTRL_ADDR		0x58
#define EDMAV2_REG_MISC_INT_STAT_ADDR		0x5c
#define EDMAV2_REG_MISC_INT_MASK_ADDR		0x60
#define EDMAV2_REG_DBG_CTRL_ADDR			0x64
#define EDMAV2_REG_DBG_DATA_ADDR			0x68
#define EDMAV2_REG_TX_TIMEOUT_THRESH_ADDR		0x6c
#define EDMAV2_REG_REQ0_FIFO_THRESH_ADDR		0x80
#define EDMAV2_REG_WB_OS_THRESH_ADDR		0x84
#define EDMAV2_REG_MISC_ERR_QID_REG2_ADDR		0x88
#define EDMAV2_REG_TXDESC2CMPL_MAP_0_ADDR		0x8c
#define EDMAV2_REG_TXDESC2CMPL_MAP_1_ADDR		0x90
#define EDMAV2_REG_TXDESC2CMPL_MAP_2_ADDR		0x94
#define EDMAV2_REG_TXDESC2CMPL_MAP_3_ADDR		0x98
#define EDMAV2_REG_TXDESC2CMPL_MAP_4_ADDR		0x9c
#define EDMAV2_REG_TXDESC2CMPL_MAP_5_ADDR		0xa0

/* Tx descriptor ring configuration register addresses */
#define EDMAV2_REG_TXDESC_BA(n)		(0x1000 + (0x1000 * (n)))
#define EDMAV2_REG_TXDESC_PROD_IDX(n)	(0x1004 + (0x1000 * (n)))
#define EDMAV2_REG_TXDESC_CONS_IDX(n)	(0x1008 + (0x1000 * (n)))
#define EDMAV2_REG_TXDESC_RING_SIZE(n)	(0x100c + (0x1000 * (n)))
#define EDMAV2_REG_TXDESC_CTRL(n)		(0x1010 + (0x1000 * (n)))
#define EDMAV2_REG_TXDESC_BA2(n)		(0x1014 + (0x1000 * (n)))

/* RxFill ring configuration register addresses */
#define EDMAV2_REG_RXFILL_BA(n)		(0x29000 + (0x1000 * (n)))
#define EDMAV2_REG_RXFILL_PROD_IDX(n)	(0x29004 + (0x1000 * (n)))
#define EDMAV2_REG_RXFILL_CONS_IDX(n)	(0x29008 + (0x1000 * (n)))
#define EDMAV2_REG_RXFILL_RING_SIZE(n)	(0x2900c + (0x1000 * (n)))
#define EDMAV2_REG_RXFILL_BUFFER1_SIZE(n)	(0x29010 + (0x1000 * (n)))
#define EDMAV2_REG_RXFILL_FC_THRE(n)	(0x29014 + (0x1000 * (n)))
#define EDMAV2_REG_RXFILL_UGT_THRE(n)	(0x29018 + (0x1000 * (n)))
#define EDMAV2_REG_RXFILL_RING_EN(n)	(0x2901c + (0x1000 * (n)))
#define EDMAV2_REG_RXFILL_DISABLE(n)	(0x29020 + (0x1000 * (n)))
#define EDMAV2_REG_RXFILL_DISABLE_DONE(n)	(0x29024 + (0x1000 * (n)))
#define EDMAV2_REG_RXFILL_INT_STAT(n)	(0x31000 + (0x1000 * (n)))
#define EDMAV2_REG_RXFILL_INT_MASK(n)	(0x31004 + (0x1000 * (n)))

/* Rx descriptor ring configuration register addresses */
#define EDMAV2_REG_RXDESC_BA(n)		(0x39000 + (0x1000 * (n)))
#define EDMAV2_REG_RXDESC_PROD_IDX(n)	(0x39004 + (0x1000 * (n)))
#define EDMAV2_REG_RXDESC_CONS_IDX(n)	(0x39008 + (0x1000 * (n)))
#define EDMAV2_REG_RXDESC_RING_SIZE(n)	(0x3900c + (0x1000 * (n)))
#define EDMAV2_REG_RXDESC_FC_THRE(n)	(0x39010 + (0x1000 * (n)))
#define EDMAV2_REG_RXDESC_UGT_THRE(n)	(0x39014 + (0x1000 * (n)))
#define EDMAV2_REG_RXDESC_CTRL(n)		(0x39018 + (0x1000 * (n)))
#define EDMAV2_REG_RXDESC_BPC(n)		(0x3901c + (0x1000 * (n)))
#define EDMAV2_REG_RXDESC_DISABLE(n)	(0x39020 + (0x1000 * (n)))
#define EDMAV2_REG_RXDESC_DISABLE_DONE(n)	(0x39024 + (0x1000 * (n)))
#define EDMAV2_REG_RXDESC_PREHEADER_BA(n)	(0x39028 + (0x1000 * (n)))
#define EDMAV2_REG_RXDESC_INT_STAT(n)	(0x59000 + (0x1000 * (n)))
#define EDMAV2_REG_RXDESC_INT_MASK(n)	(0x59004 + (0x1000 * (n)))

#define EDMAV2_REG_RX_MOD_TIMER(n)	(0x59008 + (0x1000 * (n)))
#define EDMAV2_REG_RX_INT_CTRL(n)		(0x5900c + (0x1000 * (n)))

/* Tx completion ring configuration register addresses */
#define EDMAV2_REG_TXCMPL_BA(n)		(0x79000 + (0x1000 * (n)))
#define EDMAV2_REG_TXCMPL_PROD_IDX(n)	(0x79004 + (0x1000 * (n)))
#define EDMAV2_REG_TXCMPL_CONS_IDX(n)	(0x79008 + (0x1000 * (n)))
#define EDMAV2_REG_TXCMPL_RING_SIZE(n)	(0x7900c + (0x1000 * (n)))
#define EDMAV2_REG_TXCMPL_UGT_THRE(n)	(0x79010 + (0x1000 * (n)))
#define EDMAV2_REG_TXCMPL_CTRL(n)		(0x79014 + (0x1000 * (n)))
#define EDMAV2_REG_TXCMPL_BPC(n)		(0x79018 + (0x1000 * (n)))

#define EDMAV2_REG_TX_INT_STAT(n)		(0x99000 + (0x1000 * (n)))
#define EDMAV2_REG_TX_INT_MASK(n)		(0x99004 + (0x1000 * (n)))
#define EDMAV2_REG_TX_MOD_TIMER(n)	(0x99008 + (0x1000 * (n)))
#define EDMAV2_REG_TX_INT_CTRL(n)		(0x9900c + (0x1000 * (n)))

/* EDMAV2_QID2RID_TABLE_MEM register field masks */
#define EDMAV2_RX_RING_ID_QUEUE0_MASK	GENMASK(7, 0)
#define EDMAV2_RX_RING_ID_QUEUE1_MASK	GENMASK(15, 8)
#define EDMAV2_RX_RING_ID_QUEUE2_MASK	GENMASK(23, 16)
#define EDMAV2_RX_RING_ID_QUEUE3_MASK	GENMASK(31, 24)

/* EDMAV2_REG_PORT_CTRL register bit definitions */
#define EDMAV2_PORT_PAD_EN			0x1
#define EDMAV2_PORT_EDMA_EN			0x2

/* EDMAV2_REG_DMAR_CTRL register field masks */
#define EDMAV2_DMAR_REQ_PRI_MASK			GENMASK(2, 0)
#define EDMAV2_DMAR_BURST_LEN_MASK		BIT(3)
#define EDMAV2_DMAR_TXDATA_OUTSTANDING_NUM_MASK	GENMASK(8, 4)
#define EDMAV2_DMAR_TXDESC_OUTSTANDING_NUM_MASK	GENMASK(11, 9)
#define EDMAV2_DMAR_RXFILL_OUTSTANDING_NUM_MASK	GENMASK(14, 12)

#define EDMAV2_BURST_LEN_ENABLE			0

/* Tx timeout threshold */
#define EDMAV2_TX_TIMEOUT_THRESH_VAL		0xFFFF

/* Rx descriptor ring base address mask */
#define EDMAV2_RXDESC_BA_MASK			0xffffffff

/* Rx Descriptor ring pre-header base address mask */
#define EDMAV2_RXDESC_PREHEADER_BA_MASK		0xffffffff

/* Tx descriptor ring enable */
#define EDMAV2_TXDESC_TX_ENABLE			0x1

#define EDMAV2_TXDESC_CTRL_TXEN_MASK		BIT(0)
#define EDMAV2_TXDESC_CTRL_FC_GRP_ID_MASK		GENMASK(3, 1)
#define EDMAV2_TXDESC_CTRL_FC_GRP_ID_MASK_IPQ54XX	GENMASK(4, 1)

/* Tx completion ring urgent threshold mask */
#define EDMAV2_TXCMPL_LOW_THRE_MASK		0xffff
#define EDMAV2_TXCMPL_LOW_THRE_SHIFT		0

/* EDMAV2_REG_TX_MOD_TIMER mask */
#define EDMAV2_TX_MOD_TIMER_INIT_MASK		0xffff
#define EDMAV2_TX_MOD_TIMER_INIT_SHIFT		0

/* Rx fill ring flow control threshold masks */
#define EDMAV2_RXFILL_FC_XON_THRE_MASK		0x7ff
#define EDMAV2_RXFILL_FC_XON_THRE_SHIFT		12
#define EDMAV2_RXFILL_FC_XOFF_THRE_MASK		0x7ff
#define EDMAV2_RXFILL_FC_XOFF_THRE_SHIFT		0

/* Rx fill ring enable bit */
#define EDMAV2_RXFILL_RING_EN			0x1
#define EDMAV2_RXFILL_RING_DISABLE		BIT(0)

#define EDMAV2_RXDESC_PL_OFFSET_MASK		0x1ff
#define EDMAV2_RXDESC_PL_OFFSET_SHIFT		16
#define EDMAV2_RXDESC_PL_DEFAULT_VALUE		0

/* Rx descriptor ring flow control threshold masks */
#define EDMAV2_RXDESC_FC_XON_THRE_MASK		0x7ff
#define EDMAV2_RXDESC_FC_XON_THRE_SHIFT		12
#define EDMAV2_RXDESC_FC_XOFF_THRE_MASK		0x7ff
#define EDMAV2_RXDESC_FC_XOFF_THRE_SHIFT		0

/* Rx descriptor ring urgent threshold mask */
#define EDMAV2_RXDESC_LOW_THRE_MASK		0xffff
#define EDMAV2_RXDESC_LOW_THRE_SHIFT		0

/* Rx descriptor ring enable bit */
#define EDMAV2_RXDESC_RX_EN			0x1
#define EDMAV2_RXDESC_RX_DISABLE		BIT(0)

/* Tx interrupt status bit */
#define EDMAV2_TX_INT_MASK_PKT_INT		0x1

/* Rx interrupt mask */
#define EDMAV2_RXDESC_INT_MASK_PKT_INT		0x1

#define EDMAV2_MASK_INT_DISABLE			0x0
#define EDMAV2_MASK_INT_CLEAR			0x0

/* EDMAV2_REG_RX_MOD_TIMER register field masks */
#define EDMAV2_RX_MOD_TIMER_INIT_MASK		0xffff
#define EDMAV2_RX_MOD_TIMER_INIT_SHIFT		0

/* EDMA Ring mask */
#define EDMAV2_RING_DMA_MASK			0xffffffff

/* RXDESC threshold interrupt. */
#define EDMAV2_RXDESC_UGT_INT_STAT		0x2

/* RXDESC timer interrupt */
#define EDMAV2_RXDESC_PKT_INT_STAT		0x1

/* RXDESC Interrupt status mask */
#define EDMAV2_RXDESC_RING_INT_STATUS_MASK \
	(EDMAV2_RXDESC_UGT_INT_STAT | EDMAV2_RXDESC_PKT_INT_STAT)

/* TXCMPL threshold interrupt. */
#define EDMAV2_TXCMPL_UGT_INT_STAT		0x2

/* TXCMPL timer interrupt */
#define EDMAV2_TXCMPL_PKT_INT_STAT		0x1

/* TXCMPL Interrupt status mask */
#define EDMAV2_TXCMPL_RING_INT_STATUS_MASK \
	(EDMAV2_TXCMPL_UGT_INT_STAT | EDMAV2_TXCMPL_PKT_INT_STAT)

#define EDMAV2_TXCMPL_RETMODE_OPAQUE		0x0

#define EDMAV2_RXDESC_LOW_THRE			0
#define EDMAV2_RX_MOD_TIMER_INIT			1000
#define EDMAV2_RX_NE_INT_EN			0x2

#define EDMAV2_TX_MOD_TIMER			150

#define EDMAV2_TX_NE_INT_EN			0x2

/* EDMA misc error mask */
#define EDMAV2_MISC_AXI_RD_ERR_MASK		BIT(0)
#define EDMAV2_MISC_AXI_WR_ERR_MASK		BIT(1)
#define EDMAV2_MISC_RX_DESC_FIFO_FULL_MASK	BIT(2)
#define EDMAV2_MISC_RX_ERR_BUF_SIZE_MASK		BIT(3)
#define EDMAV2_MISC_TX_SRAM_FULL_MASK		BIT(4)
#define EDMAV2_MISC_TX_CMPL_BUF_FULL_MASK		BIT(5)

#define EDMAV2_MISC_DATA_LEN_ERR_MASK		BIT(6)
#define EDMAV2_MISC_TX_TIMEOUT_MASK		BIT(7)

/* EDMA txdesc2cmpl map */
#define EDMAV2_TXDESC2CMPL_MAP_TXDESC_MASK		0x1F

/* EDMA rxdesc2fill map */
#define EDMAV2_RXDESC2FILL_MAP_RXDESC_MASK	0x7

#endif /* __EDMAV2_REGS_H__ */
