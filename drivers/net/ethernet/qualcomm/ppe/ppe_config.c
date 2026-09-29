// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

/* PPE HW initialization configs such as BM(buffer management),
 * QM(queue management) and scheduler configs.
 */

#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/bits.h>
#include <linux/device.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/log2.h>
#include <linux/regmap.h>
#include <linux/spinlock.h>
#include <linux/unaligned.h>

#include "ppe.h"
#include "ppe_config.h"
#include "ppe_regs.h"

#define PPE_QUEUE_SCH_PRI_NUM		8

/**
 * struct ppe_bm_port_config - PPE BM port configuration.
 * @port_id_start: The fist BM port ID to configure.
 * @port_id_end: The last BM port ID to configure.
 * @pre_alloc: BM port dedicated buffer number.
 * @in_fly_buf: Buffer number for receiving the packet after pause frame sent.
 * @ceil: Ceil to generate the back pressure.
 * @weight: Weight value.
 * @resume_offset: Resume offset from the threshold value.
 * @resume_ceil: Ceil to resume from the back pressure state.
 * @dynamic: Dynamic threshold used or not.
 *
 * The is for configuring the threshold that impacts the port
 * flow control.
 */
struct ppe_bm_port_config {
	unsigned int port_id_start;
	unsigned int port_id_end;
	unsigned int pre_alloc;
	unsigned int in_fly_buf;
	unsigned int ceil;
	unsigned int weight;
	unsigned int resume_offset;
	unsigned int resume_ceil;
	bool dynamic;
};

/**
 * struct ppe_qm_queue_config - PPE queue config.
 * @queue_start: PPE start of queue ID.
 * @queue_end: PPE end of queue ID.
 * @prealloc_buf: Queue dedicated buffer number.
 * @ceil: Ceil to start drop packet from queue.
 * @weight: Weight value.
 * @resume_offset: Resume offset from the threshold.
 * @dynamic: Threshold value is decided dynamically or statically.
 *
 * Queue configuration decides the threshold to drop packet from PPE
 * hardware queue.
 */
struct ppe_qm_queue_config {
	unsigned int queue_start;
	unsigned int queue_end;
	unsigned int prealloc_buf;
	unsigned int ceil;
	unsigned int weight;
	unsigned int resume_offset;
	bool dynamic;
};

/**
 * enum ppe_scheduler_direction - PPE scheduler direction for packet.
 * @PPE_SCH_INGRESS: Scheduler for the packet on ingress,
 * @PPE_SCH_EGRESS: Scheduler for the packet on egress,
 */
enum ppe_scheduler_direction {
	PPE_SCH_INGRESS = 0,
	PPE_SCH_EGRESS = 1,
};

/**
 * struct ppe_scheduler_bm_config - PPE arbitration for buffer config.
 * @valid: Arbitration entry valid or not.
 * @dir: Arbitration entry for egress or ingress.
 * @port: Port ID to use arbitration entry.
 * @backup_port_valid: Backup port valid or not.
 * @backup_port: Backup port ID to use.
 *
 * Configure the scheduler settings for accessing and releasing the PPE buffers.
 */
struct ppe_scheduler_bm_config {
	bool valid;
	enum ppe_scheduler_direction dir;
	unsigned int port;
	bool backup_port_valid;
	unsigned int backup_port;
};

/**
 * struct ppe_scheduler_qm_config - PPE arbitration for scheduler config.
 * @ensch_port_bmp: Port bit map for enqueue scheduler.
 * @ensch_port: Port ID to enqueue scheduler.
 * @desch_port: Port ID to dequeue scheduler.
 * @desch_backup_port_valid: Dequeue for the backup port valid or not.
 * @desch_backup_port: Backup port ID to dequeue scheduler.
 *
 * Configure the scheduler settings for enqueuing and dequeuing packets on
 * the PPE port.
 */
struct ppe_scheduler_qm_config {
	unsigned int ensch_port_bmp;
	unsigned int ensch_port;
	unsigned int desch_port;
	bool desch_backup_port_valid;
	unsigned int desch_backup_port;
};

/**
 * struct ppe_scheduler_port_config - PPE port scheduler config.
 * @port: Port ID to be scheduled.
 * @flow_level: Scheduler flow level or not.
 * @node_id: Node ID, for level 0, queue ID is used.
 * @loop_num: Loop number of scheduler config.
 * @pri_max: Max priority configured.
 * @flow_id: Strict priority ID.
 * @drr_node_id: Node ID for scheduler.
 *
 * PPE port scheduler configuration which decides the priority in the
 * packet scheduler for the egress port.
 */
struct ppe_scheduler_port_config {
	unsigned int port;
	bool flow_level;
	unsigned int node_id;
	unsigned int loop_num;
	unsigned int pri_max;
	unsigned int flow_id;
	unsigned int drr_node_id;
};

/**
 * struct ppe_port_schedule_resource - PPE port scheduler resource.
 * @ucastq_start: Unicast queue start ID.
 * @ucastq_end: Unicast queue end ID.
 * @mcastq_start: Multicast queue start ID.
 * @mcastq_end: Multicast queue end ID.
 * @flow_id_start: Flow start ID.
 * @flow_id_end: Flow end ID.
 * @l0node_start: Scheduler node start ID for queue level.
 * @l0node_end: Scheduler node end ID for queue level.
 * @l1node_start: Scheduler node start ID for flow level.
 * @l1node_end: Scheduler node end ID for flow level.
 *
 * PPE scheduler resource allocated among the PPE ports.
 */
struct ppe_port_schedule_resource {
	unsigned int ucastq_start;
	unsigned int ucastq_end;
	unsigned int mcastq_start;
	unsigned int mcastq_end;
	unsigned int flow_id_start;
	unsigned int flow_id_end;
	unsigned int l0node_start;
	unsigned int l0node_end;
	unsigned int l1node_start;
	unsigned int l1node_end;
};

/* The buffer configurations per PPE port. There are 15 BM ports and
 * 4 BM groups supported by PPE. BM port (0-7) is for EDMA port 0,
 * BM port (8-13) is for PPE physical port 1-6 and BM port 14 is for
 * EIP port.
 */
static const struct ppe_bm_port_config ipq9574_ppe_bm_port_config[] = {
	{
		/* Buffer configuration for the BM port ID 0 of EDMA. */
		.port_id_start	= 0,
		.port_id_end	= 0,
		.pre_alloc	= 0,
		.in_fly_buf	= 100,
		.ceil		= 1146,
		.weight		= 7,
		.resume_offset	= 8,
		.resume_ceil	= 0,
		.dynamic	= true,
	},
	{
		/* Buffer configuration for the BM port ID 1-7 of EDMA. */
		.port_id_start	= 1,
		.port_id_end	= 7,
		.pre_alloc	= 0,
		.in_fly_buf	= 100,
		.ceil		= 250,
		.weight		= 4,
		.resume_offset	= 36,
		.resume_ceil	= 0,
		.dynamic	= true,
	},
	{
		/* Buffer configuration for the BM port ID 8-13 of PPE ports. */
		.port_id_start	= 8,
		.port_id_end	= 13,
		.pre_alloc	= 0,
		.in_fly_buf	= 128,
		.ceil		= 250,
		.weight		= 4,
		.resume_offset	= 36,
		.resume_ceil	= 0,
		.dynamic	= true,
	},
	{
		/* Buffer configuration for the BM port ID 14 of EIP. */
		.port_id_start	= 14,
		.port_id_end	= 14,
		.pre_alloc	= 0,
		.in_fly_buf	= 40,
		.ceil		= 250,
		.weight		= 4,
		.resume_offset	= 36,
		.resume_ceil	= 0,
		.dynamic	= true,
	},
};

static const struct ppe_bm_port_config ipq5424_ppe_bm_port_config[] = {
	{
		.port_id_start	= 0,
		.port_id_end	= 0,
		.pre_alloc	= 0,
		.in_fly_buf	= 100,
		.ceil		= 1146,
		.weight		= 7,
		.resume_offset	= 8,
		.resume_ceil	= 0,
		.dynamic	= true,
	},
	{
		.port_id_start	= 0,
		.port_id_end	= 15,
		.pre_alloc	= 0,
		.in_fly_buf	= 100,
		.ceil		= 250,
		.weight		= 4,
		.resume_offset	= 36,
		.resume_ceil	= 0,
		.dynamic	= true,
	},
	{
		.port_id_start	= 16,
		.port_id_end	= 18,
		.pre_alloc	= 0,
		.in_fly_buf	= 128,
		.ceil		= 250,
		.weight		= 4,
		.resume_offset	= 36,
		.resume_ceil	= 0,
		.dynamic	= true,
	},
};

/* Default QM settings for unicast and multicast queues for IPQ9754. */
static const struct ppe_qm_queue_config ipq9574_ppe_qm_queue_config[] = {
	{
		/* QM settings for unicast queues 0 to 255. */
		.queue_start	= 0,
		.queue_end	= 255,
		.prealloc_buf	= 0,
		.ceil		= 1200,
		.weight		= 7,
		.resume_offset	= 36,
		.dynamic	= true,
	},
	{
		/* QM settings for multicast queues 256 to 299. */
		.queue_start	= 256,
		.queue_end	= 299,
		.prealloc_buf	= 0,
		.ceil		= 250,
		.weight		= 0,
		.resume_offset	= 36,
		.dynamic	= false,
	},
};

/* PPE scheduler configuration for BM includes multiple entries. Each entry
 * indicates the primary port to be assigned the buffers for the ingress or
 * to release the buffers for the egress. Backup port ID will be used when
 * the primary port ID is down.
 */
static const struct ppe_scheduler_bm_config ipq9574_ppe_sch_bm_config[] = {
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS,  0, false, 0},
	{true, PPE_SCH_INGRESS, 5, false, 0},
	{true, PPE_SCH_EGRESS,  5, false, 0},
	{true, PPE_SCH_INGRESS, 6, false, 0},
	{true, PPE_SCH_EGRESS,  6, false, 0},
	{true, PPE_SCH_INGRESS, 1, false, 0},
	{true, PPE_SCH_EGRESS,  1, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS,  0, false, 0},
	{true, PPE_SCH_INGRESS, 5, false, 0},
	{true, PPE_SCH_EGRESS,  5, false, 0},
	{true, PPE_SCH_INGRESS, 6, false, 0},
	{true, PPE_SCH_EGRESS,  6, false, 0},
	{true, PPE_SCH_INGRESS, 7, false, 0},
	{true, PPE_SCH_EGRESS,  7, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS,  0, false, 0},
	{true, PPE_SCH_INGRESS, 1, false, 0},
	{true, PPE_SCH_EGRESS,  1, false, 0},
	{true, PPE_SCH_INGRESS, 5, false, 0},
	{true, PPE_SCH_EGRESS,  5, false, 0},
	{true, PPE_SCH_INGRESS, 6, false, 0},
	{true, PPE_SCH_EGRESS,  6, false, 0},
	{true, PPE_SCH_INGRESS, 2, false, 0},
	{true, PPE_SCH_EGRESS,  2, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS,  0, false, 0},
	{true, PPE_SCH_INGRESS, 5, false, 0},
	{true, PPE_SCH_EGRESS,  5, false, 0},
	{true, PPE_SCH_INGRESS, 6, false, 0},
	{true, PPE_SCH_EGRESS,  6, false, 0},
	{true, PPE_SCH_INGRESS, 1, false, 0},
	{true, PPE_SCH_EGRESS,  1, false, 0},
	{true, PPE_SCH_INGRESS, 3, false, 0},
	{true, PPE_SCH_EGRESS,  3, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS,  0, false, 0},
	{true, PPE_SCH_INGRESS, 5, false, 0},
	{true, PPE_SCH_EGRESS,  5, false, 0},
	{true, PPE_SCH_INGRESS, 6, false, 0},
	{true, PPE_SCH_EGRESS,  6, false, 0},
	{true, PPE_SCH_INGRESS, 7, false, 0},
	{true, PPE_SCH_EGRESS,  7, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS,  0, false, 0},
	{true, PPE_SCH_INGRESS, 1, false, 0},
	{true, PPE_SCH_EGRESS,  1, false, 0},
	{true, PPE_SCH_INGRESS, 5, false, 0},
	{true, PPE_SCH_EGRESS,  5, false, 0},
	{true, PPE_SCH_INGRESS, 6, false, 0},
	{true, PPE_SCH_EGRESS,  6, false, 0},
	{true, PPE_SCH_INGRESS, 4, false, 0},
	{true, PPE_SCH_EGRESS,  4, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS,  0, false, 0},
	{true, PPE_SCH_INGRESS, 5, false, 0},
	{true, PPE_SCH_EGRESS,  5, false, 0},
	{true, PPE_SCH_INGRESS, 6, false, 0},
	{true, PPE_SCH_EGRESS,  6, false, 0},
	{true, PPE_SCH_INGRESS, 1, false, 0},
	{true, PPE_SCH_EGRESS,  1, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS,  0, false, 0},
	{true, PPE_SCH_INGRESS, 5, false, 0},
	{true, PPE_SCH_EGRESS,  5, false, 0},
	{true, PPE_SCH_INGRESS, 6, false, 0},
	{true, PPE_SCH_EGRESS,  6, false, 0},
	{true, PPE_SCH_INGRESS, 2, false, 0},
	{true, PPE_SCH_EGRESS,  2, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS,  0, false, 0},
	{true, PPE_SCH_INGRESS, 7, false, 0},
	{true, PPE_SCH_EGRESS,  7, false, 0},
	{true, PPE_SCH_INGRESS, 5, false, 0},
	{true, PPE_SCH_EGRESS,  5, false, 0},
	{true, PPE_SCH_INGRESS, 6, false, 0},
	{true, PPE_SCH_EGRESS,  6, false, 0},
	{true, PPE_SCH_INGRESS, 1, false, 0},
	{true, PPE_SCH_EGRESS,  1, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS,  0, false, 0},
	{true, PPE_SCH_INGRESS, 5, false, 0},
	{true, PPE_SCH_EGRESS,  5, false, 0},
	{true, PPE_SCH_INGRESS, 6, false, 0},
	{true, PPE_SCH_EGRESS,  6, false, 0},
	{true, PPE_SCH_INGRESS, 3, false, 0},
	{true, PPE_SCH_EGRESS,  3, false, 0},
	{true, PPE_SCH_INGRESS, 1, false, 0},
	{true, PPE_SCH_EGRESS,  1, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS,  0, false, 0},
	{true, PPE_SCH_INGRESS, 5, false, 0},
	{true, PPE_SCH_EGRESS,  5, false, 0},
	{true, PPE_SCH_INGRESS, 6, false, 0},
	{true, PPE_SCH_EGRESS,  6, false, 0},
	{true, PPE_SCH_INGRESS, 4, false, 0},
	{true, PPE_SCH_EGRESS,  4, false, 0},
	{true, PPE_SCH_INGRESS, 7, false, 0},
	{true, PPE_SCH_EGRESS,  7, false, 0},
};

/* PPE scheduler configuration for QM includes multiple entries. Each entry
 * contains ports to be dispatched for enqueueing and dequeueing. The backup
 * port for dequeueing is supported to be used when the primary port for
 * dequeueing is down.
 */
static const struct ppe_scheduler_qm_config ipq9574_ppe_sch_qm_config[] = {
	{0x98, 6, 0, true, 1},
	{0x94, 5, 6, true, 3},
	{0x86, 0, 5, true, 4},
	{0x8C, 1, 6, true, 0},
	{0x1C, 7, 5, true, 1},
	{0x98, 2, 6, true, 0},
	{0x1C, 5, 7, true, 1},
	{0x34, 3, 6, true, 0},
	{0x8C, 4, 5, true, 1},
	{0x98, 2, 6, true, 0},
	{0x8C, 5, 4, true, 1},
	{0xA8, 0, 6, true, 2},
	{0x98, 5, 1, true, 0},
	{0x98, 6, 5, true, 2},
	{0x89, 1, 6, true, 4},
	{0xA4, 3, 0, true, 1},
	{0x8C, 5, 6, true, 4},
	{0xA8, 0, 2, true, 1},
	{0x98, 6, 5, true, 0},
	{0xC4, 4, 3, true, 1},
	{0x94, 6, 5, true, 0},
	{0x1C, 7, 6, true, 1},
	{0x98, 2, 5, true, 0},
	{0x1C, 6, 7, true, 1},
	{0x1C, 5, 6, true, 0},
	{0x94, 3, 5, true, 1},
	{0x8C, 4, 6, true, 0},
	{0x94, 1, 5, true, 3},
	{0x94, 6, 1, true, 0},
	{0xD0, 3, 5, true, 2},
	{0x98, 6, 0, true, 1},
	{0x94, 5, 6, true, 3},
	{0x94, 1, 5, true, 0},
	{0x98, 2, 6, true, 1},
	{0x8C, 4, 5, true, 0},
	{0x1C, 7, 6, true, 1},
	{0x8C, 0, 5, true, 4},
	{0x89, 1, 6, true, 2},
	{0x98, 5, 0, true, 1},
	{0x94, 6, 5, true, 3},
	{0x92, 0, 6, true, 2},
	{0x98, 1, 5, true, 0},
	{0x98, 6, 2, true, 1},
	{0xD0, 0, 5, true, 3},
	{0x94, 6, 0, true, 1},
	{0x8C, 5, 6, true, 4},
	{0x8C, 1, 5, true, 0},
	{0x1C, 6, 7, true, 1},
	{0x1C, 5, 6, true, 0},
	{0xB0, 2, 3, true, 1},
	{0xC4, 4, 5, true, 0},
	{0x8C, 6, 4, true, 1},
	{0xA4, 3, 6, true, 0},
	{0x1C, 5, 7, true, 1},
	{0x4C, 0, 5, true, 4},
	{0x8C, 6, 0, true, 1},
	{0x34, 7, 6, true, 3},
	{0x94, 5, 0, true, 1},
	{0x98, 6, 5, true, 2},
};

static const struct ppe_scheduler_bm_config ipq5424_ppe_sch_bm_config[] = {
	{1, 0, 3, 0, 0},
	{1, 1, 0, 0, 0},
	{1, 0, 2, 0, 0},
	{1, 1, 3, 1, 1},
	{1, 0, 0, 0, 0},
	{1, 1, 2, 0, 0},
	{1, 0, 1, 0, 0},
	{1, 1, 0, 0, 0},
	{1, 0, 3, 0, 0},
	{1, 1, 1, 1, 2},
	{1, 0, 2, 0, 0},
	{1, 1, 3, 0, 0},
	{1, 0, 0, 0, 0},
	{1, 1, 2, 1, 1},
	{1, 0, 1, 0, 0},
	{1, 1, 0, 0, 0},
	{1, 0, 3, 0, 0},
	{1, 1, 1, 0, 0},
	{1, 0, 2, 0, 0},
	{1, 1, 3, 0, 0},
	{1, 0, 0, 0, 0},
	{1, 1, 2, 1, 1},
	{1, 0, 1, 0, 0},
	{1, 1, 0, 0, 0},
	{1, 0, 3, 0, 0},
	{1, 1, 3, 1, 2},
	{1, 0, 2, 0, 0},
	{1, 1, 1, 0, 0},
	{1, 0, 0, 0, 0},
	{1, 1, 2, 1, 3},
	{1, 0, 1, 0, 0},
	{1, 1, 0, 0, 0},
	{1, 0, 3, 0, 0},
	{1, 1, 1, 1, 2},
	{1, 0, 0, 0, 0},
	{1, 1, 3, 0, 0},
	{1, 0, 2, 0, 0},
	{1, 1, 0, 0, 0},
	{1, 0, 1, 0, 0},
	{1, 1, 2, 1, 3},
	{1, 0, 0, 0, 0},
	{1, 1, 1, 0, 0},
	{1, 0, 3, 0, 0},
	{1, 1, 0, 0, 0},
	{1, 0, 2, 0, 0},
	{1, 1, 3, 1, 1},
	{1, 0, 0, 0, 0},
	{1, 1, 2, 0, 0},
	{1, 0, 1, 0, 0},
	{1, 1, 0, 0, 0},
	{1, 0, 3, 0, 0},
	{1, 1, 1, 1, 2},
	{1, 0, 2, 0, 0},
	{1, 1, 3, 0, 0},
	{1, 0, 0, 0, 0},
	{1, 1, 2, 1, 1},
	{1, 0, 1, 0, 0},
	{1, 1, 0, 0, 0},
	{1, 0, 3, 0, 0},
	{1, 1, 1, 1, 2},
	{1, 0, 2, 0, 0},
	{1, 1, 3, 0, 0},
	{1, 0, 0, 0, 0},
	{1, 1, 2, 1, 1},
	{1, 0, 1, 0, 0},
	{1, 1, 0, 0, 0},
	{1, 0, 3, 0, 0},
	{1, 1, 1, 1, 3},
	{1, 0, 2, 0, 0},
	{1, 1, 2, 0, 0},
	{1, 0, 0, 0, 0},
	{1, 1, 3, 1, 1},
	{1, 0, 1, 0, 0},
	{1, 1, 0, 0, 0},
	{1, 0, 3, 0, 0},
	{1, 1, 1, 1, 2},
	{1, 0, 0, 0, 0},
	{1, 1, 3, 0, 0},
	{1, 0, 2, 0, 0},
	{1, 1, 0, 0, 0},
	{1, 0, 1, 0, 0},
	{1, 1, 2, 1, 3},
	{1, 0, 0, 0, 0},
	{1, 1, 1, 0, 0},
	{1, 0, 3, 0, 0},
	{1, 1, 0, 0, 0},
	{1, 0, 2, 0, 0},
	{1, 1, 3, 1, 1},
	{1, 0, 0, 0, 0},
	{1, 1, 2, 0, 0},
	{1, 0, 1, 0, 0},
	{1, 1, 0, 0, 0},
	{1, 0, 3, 0, 0},
	{1, 1, 1, 1, 3},
	{1, 0, 2, 0, 0},
	{1, 1, 2, 0, 0},
	{1, 0, 0, 0, 0},
	{1, 1, 3, 1, 1},
	{1, 0, 1, 0, 0},
	{1, 1, 0, 0, 0},
	{1, 0, 3, 0, 0},
	{1, 1, 1, 1, 2},
	{1, 0, 2, 0, 0},
	{1, 1, 3, 0, 0},
	{1, 0, 0, 0, 0},
	{1, 1, 2, 1, 1},
	{1, 0, 1, 0, 0},
	{1, 1, 0, 0, 0},
	{1, 0, 3, 0, 0},
	{1, 1, 3, 1, 2},
	{1, 0, 2, 0, 0},
	{1, 1, 1, 0, 0},
	{1, 0, 0, 0, 0},
	{1, 1, 2, 1, 3},
	{1, 0, 1, 0, 0},
	{1, 1, 0, 0, 0},
	{1, 0, 3, 0, 0},
	{1, 1, 1, 1, 2},
	{1, 0, 0, 0, 0},
	{1, 1, 3, 0, 0},
	{1, 0, 2, 0, 0},
	{1, 1, 0, 0, 0},
	{1, 0, 1, 0, 0},
	{1, 1, 2, 1, 3},
	{1, 0, 0, 0, 0},
	{1, 1, 1, 0, 0},
};

static const struct ppe_scheduler_qm_config ipq5424_ppe_sch_qm_config[] = {
	{0x0, 3, 2, false, 0},
	{0x0, 1, 3, false, 0},
	{0x0, 0, 1, false, 0},
	{0x0, 2, 0, false, 0},
	{0x0, 1, 2, false, 0},
	{0x0, 3, 1, true, 0},
	{0x0, 3, 2, false, 0},
	{0x0, 1, 0, false, 0},
	{0x0, 2, 1, false, 0},
	{0x0, 2, 0, false, 0},
	{0x0, 1, 2, false, 0},
	{0x0, 3, 1, true, 0},
	{0x0, 3, 2, false, 0},
	{0x0, 1, 3, false, 0},
	{0x0, 0, 1, false, 0},
	{0x0, 2, 0, false, 0},
	{0x0, 1, 2, false, 0},
	{0x0, 3, 1, true, 0},
	{0x0, 3, 2, false, 0},
	{0x0, 1, 0, false, 0},
	{0x0, 2, 1, false, 0},
	{0x0, 2, 0, false, 0},
	{0x0, 1, 2, false, 0},
	{0x0, 0, 1, false, 0},
	{0x0, 2, 0, false, 0},
	{0x0, 3, 1, false, 0},
	{0x0, 2, 0, false, 0},
	{0x0, 1, 2, false, 0},
	{0x0, 1, 0, false, 0},
	{0x0, 2, 1, false, 0},
	{0x0, 3, 2, true, 0},
	{0x0, 3, 1, false, 0},
	{0x0, 2, 0, false, 0},
	{0x0, 1, 2, false, 0},
	{0x0, 1, 3, false, 0},
	{0x0, 0, 1, false, 0},
	{0x0, 3, 2, true, 0},
	{0x0, 3, 1, false, 0},
	{0x0, 2, 0, false, 0},
	{0x0, 1, 2, false, 0},
	{0x0, 1, 0, false, 0},
	{0x0, 2, 1, false, 0},
	{0x0, 0, 2, false, 0},
	{0x0, 3, 1, false, 0},
	{0x0, 2, 0, false, 0},
	{0x0, 1, 2, false, 0},
	{0x0, 1, 0, false, 0},
	{0x0, 2, 1, false, 0},
	{0x0, 0, 2, false, 0},
	{0x0, 3, 0, false, 0},
	{0x0, 3, 1, false, 0},
	{0x0, 2, 3, false, 0},
	{0x0, 0, 2, false, 0},
	{0x0, 1, 0, false, 0},
	{0x0, 2, 1, false, 0},
	{0x0, 3, 2, true, 0},
	{0x0, 3, 1, false, 0},
	{0x0, 2, 3, false, 0},
	{0x0, 0, 2, false, 0},
	{0x0, 1, 0, false, 0},
	{0x0, 2, 1, false, 0},
	{0x0, 0, 2, false, 0},
	{0x0, 3, 0, false, 0},
};

static const struct ppe_bm_port_config ipq8074_ppe_bm_port_config[] = {
	{
		/* Buffer configuration for the BM port ID 0-7 of EDMA. */
		.port_id_start	= 0,
		.port_id_end	= 7,
		.pre_alloc	= 0,
		.in_fly_buf	= 100,
		.ceil		= 250,
		.weight		= 4,
		.resume_offset	= 36,
		.resume_ceil	= 0,
		.dynamic	= true,
	},
	{
		/* Buffer configuration for the BM port ID 8-13 of PPE ports. */
		.port_id_start	= 8,
		.port_id_end	= 13,
		.pre_alloc	= 0,
		.in_fly_buf	= 128,
		.ceil		= 250,
		.weight		= 4,
		.resume_offset	= 36,
		.resume_ceil	= 0,
		.dynamic	= true,
	},
	{
		/* Buffer configuration for the BM port ID 14 of EIP. */
		.port_id_start	= 14,
		.port_id_end	= 14,
		.pre_alloc	= 0,
		.in_fly_buf	= 40,
		.ceil		= 250,
		.weight		= 4,
		.resume_offset	= 36,
		.resume_ceil	= 0,
		.dynamic	= true,
	},
};

static const struct ppe_qm_queue_config ipq8074_ppe_qm_queue_config[] = {
	{
		/* QM settings for unicast queues 0 to 255. */
		.queue_start	= 0,
		.queue_end	= 255,
		.prealloc_buf	= 0,
		.ceil		= 400,
		.weight		= 4,
		.resume_offset	= 36,
		.dynamic	= true,
	},
	{
		/* QM settings for multicast queues 256 to 299. */
		.queue_start	= 256,
		.queue_end	= 299,
		.prealloc_buf	= 0,
		.ceil		= 250,
		.weight		= 0,
		.resume_offset	= 36,
		.dynamic	= false,
	},
};

static const struct ppe_scheduler_bm_config ipq8074_ppe_sch_bm_config[] = {
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS, 5, false, 0},
	{true, PPE_SCH_INGRESS, 5, false, 0},
	{true, PPE_SCH_EGRESS, 7, false, 0},
	{true, PPE_SCH_INGRESS, 1, false, 0},
	{true, PPE_SCH_EGRESS, 6, false, 0},
	{true, PPE_SCH_INGRESS, 6, false, 0},
	{true, PPE_SCH_EGRESS, 0, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS, 7, false, 0},
	{true, PPE_SCH_INGRESS, 7, false, 0},
	{true, PPE_SCH_EGRESS, 5, false, 0},
	{true, PPE_SCH_INGRESS, 5, false, 0},
	{true, PPE_SCH_EGRESS, 0, false, 0},
	{true, PPE_SCH_INGRESS, 6, false, 0},
	{true, PPE_SCH_EGRESS, 6, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS, 3, false, 0},
	{true, PPE_SCH_INGRESS, 2, false, 0},
	{true, PPE_SCH_EGRESS, 7, false, 0},
	{true, PPE_SCH_INGRESS, 7, false, 0},
	{true, PPE_SCH_EGRESS, 5, false, 0},
	{true, PPE_SCH_INGRESS, 5, false, 0},
	{true, PPE_SCH_EGRESS, 0, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS, 6, false, 0},
	{true, PPE_SCH_INGRESS, 6, false, 0},
	{true, PPE_SCH_EGRESS, 0, false, 0},
	{true, PPE_SCH_INGRESS, 7, false, 0},
	{true, PPE_SCH_EGRESS, 7, false, 0},
	{true, PPE_SCH_INGRESS, 5, false, 0},
	{true, PPE_SCH_EGRESS, 5, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS, 0, false, 0},
	{true, PPE_SCH_INGRESS, 6, false, 0},
	{true, PPE_SCH_EGRESS, 6, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS, 0, false, 0},
	{true, PPE_SCH_INGRESS, 7, false, 0},
	{true, PPE_SCH_EGRESS, 7, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS, 5, false, 0},
	{true, PPE_SCH_INGRESS, 5, false, 0},
	{true, PPE_SCH_EGRESS, 4, false, 0},
	{true, PPE_SCH_INGRESS, 6, false, 0},
	{true, PPE_SCH_EGRESS, 6, false, 0},
	{true, PPE_SCH_INGRESS, 7, false, 0},
	{true, PPE_SCH_EGRESS, 0, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS, 7, false, 0},
	{true, PPE_SCH_INGRESS, 4, false, 0},
	{true, PPE_SCH_EGRESS, 5, false, 0},
	{true, PPE_SCH_INGRESS, 5, false, 0},
	{true, PPE_SCH_EGRESS, 0, false, 0},
	{true, PPE_SCH_INGRESS, 6, false, 0},
	{true, PPE_SCH_EGRESS, 6, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS, 1, false, 0},
	{true, PPE_SCH_INGRESS, 7, false, 0},
	{true, PPE_SCH_EGRESS, 7, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS, 5, false, 0},
	{true, PPE_SCH_INGRESS, 5, false, 0},
	{true, PPE_SCH_EGRESS, 0, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS, 6, false, 0},
	{true, PPE_SCH_INGRESS, 6, false, 0},
	{true, PPE_SCH_EGRESS, 0, false, 0},
	{true, PPE_SCH_INGRESS, 7, false, 0},
	{true, PPE_SCH_EGRESS, 7, false, 0},
	{true, PPE_SCH_INGRESS, 5, false, 0},
	{true, PPE_SCH_EGRESS, 5, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS, 0, false, 0},
	{true, PPE_SCH_INGRESS, 6, false, 0},
	{true, PPE_SCH_EGRESS, 6, false, 0},
	{true, PPE_SCH_INGRESS, 3, false, 0},
	{true, PPE_SCH_EGRESS, 0, false, 0},
	{true, PPE_SCH_INGRESS, 7, false, 0},
	{true, PPE_SCH_EGRESS, 7, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS, 5, false, 0},
	{true, PPE_SCH_INGRESS, 5, false, 0},
	{true, PPE_SCH_EGRESS, 0, false, 0},
	{true, PPE_SCH_INGRESS, 6, false, 0},
	{true, PPE_SCH_EGRESS, 6, false, 0},
	{true, PPE_SCH_INGRESS, 7, false, 0},
	{true, PPE_SCH_EGRESS, 2, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS, 7, false, 0},
	{true, PPE_SCH_INGRESS, 5, false, 0},
	{true, PPE_SCH_EGRESS, 5, false, 0},
	{true, PPE_SCH_INGRESS, 6, false, 0},
	{true, PPE_SCH_EGRESS, 0, false, 0},
	{true, PPE_SCH_INGRESS, 7, false, 0},
	{true, PPE_SCH_EGRESS, 6, false, 0},
};

static const struct ppe_scheduler_qm_config ipq8074_ppe_sch_qm_config[] = {
	{0xEE, 6, 0, false, 0},
	{0xDE, 4, 5, false, 0},
	{0x9F, 0, 6, false, 0},
	{0xBE, 5, 0, false, 0},
	{0x7E, 6, 7, false, 0},
	{0x5F, 0, 5, false, 0},
	{0x9F, 7, 6, false, 0},
	{0xBE, 5, 0, false, 0},
	{0xFC, 6, 1, false, 0},
	{0xDD, 0, 5, false, 0},
	{0xDE, 1, 0, false, 0},
	{0xBE, 5, 6, false, 0},
	{0xBB, 0, 2, false, 0},
	{0xDB, 6, 5, false, 0},
	{0xDE, 2, 0, false, 0},
	{0xBE, 5, 6, false, 0},
	{0x3F, 0, 7, false, 0},
	{0x7E, 6, 0, false, 0},
	{0xDE, 7, 5, false, 0},
	{0x9F, 0, 6, false, 0},
	{0xB7, 5, 3, false, 0},
	{0xF6, 6, 0, false, 0},
	{0xDE, 3, 5, false, 0},
	{0x9F, 0, 6, false, 0},
	{0xBE, 5, 0, false, 0},
	{0xEE, 6, 4, false, 0},
	{0xCF, 0, 5, false, 0},
	{0x9F, 4, 6, false, 0},
	{0xBE, 5, 0, false, 0},
	{0x7E, 6, 7, false, 0},
	{0x5F, 0, 5, false, 0},
	{0xDE, 7, 0, false, 0},
	{0xBE, 5, 6, false, 0},
	{0xBD, 0, 1, false, 0},
	{0xDD, 6, 5, false, 0},
	{0xDE, 1, 0, false, 0},
	{0xBE, 5, 6, false, 0},
	{0xBB, 0, 2, false, 0},
	{0xFA, 6, 0, false, 0},
	{0xDE, 2, 5, false, 0},
	{0x9F, 0, 6, false, 0},
	{0x3F, 5, 7, false, 0},
	{0x7E, 6, 0, false, 0},
	{0xDE, 7, 5, false, 0},
	{0x9F, 0, 6, false, 0},
	{0xB7, 5, 3, false, 0},
	{0xF6, 6, 0, false, 0},
	{0xDE, 3, 5, false, 0},
	{0x9F, 0, 6, false, 0},
	{0xAF, 5, 4, false, 0},
};

static const struct ppe_scheduler_port_config ipq9574_ppe_port_sch_config[] = {
	{
		.port		= 0,
		.flow_level	= true,
		.node_id	= 0,
		.loop_num	= 1,
		.pri_max	= 1,
		.flow_id	= 0,
		.drr_node_id	= 0,
	},
	{
		.port		= 0,
		.flow_level	= false,
		.node_id	= 0,
		.loop_num	= 8,
		.pri_max	= 8,
		.flow_id	= 0,
		.drr_node_id	= 0,
	},
	{
		.port		= 0,
		.flow_level	= false,
		.node_id	= 8,
		.loop_num	= 8,
		.pri_max	= 8,
		.flow_id	= 0,
		.drr_node_id	= 0,
	},
	{
		.port		= 0,
		.flow_level	= false,
		.node_id	= 16,
		.loop_num	= 8,
		.pri_max	= 8,
		.flow_id	= 0,
		.drr_node_id	= 0,
	},
	{
		.port		= 0,
		.flow_level	= false,
		.node_id	= 24,
		.loop_num	= 8,
		.pri_max	= 8,
		.flow_id	= 0,
		.drr_node_id	= 0,
	},
	{
		.port		= 0,
		.flow_level	= false,
		.node_id	= 32,
		.loop_num	= 8,
		.pri_max	= 8,
		.flow_id	= 0,
		.drr_node_id	= 0,
	},
	{
		.port		= 0,
		.flow_level	= false,
		.node_id	= 40,
		.loop_num	= 8,
		.pri_max	= 8,
		.flow_id	= 0,
		.drr_node_id	= 0,
	},
	{
		.port		= 0,
		.flow_level	= false,
		.node_id	= 48,
		.loop_num	= 8,
		.pri_max	= 8,
		.flow_id	= 0,
		.drr_node_id	= 0,
	},
	{
		.port		= 0,
		.flow_level	= false,
		.node_id	= 56,
		.loop_num	= 8,
		.pri_max	= 8,
		.flow_id	= 0,
		.drr_node_id	= 0,
	},
	{
		.port		= 0,
		.flow_level	= false,
		.node_id	= 256,
		.loop_num	= 8,
		.pri_max	= 8,
		.flow_id	= 0,
		.drr_node_id	= 0,
	},
	{
		.port		= 0,
		.flow_level	= false,
		.node_id	= 264,
		.loop_num	= 8,
		.pri_max	= 8,
		.flow_id	= 0,
		.drr_node_id	= 0,
	},
	{
		.port		= 1,
		.flow_level	= true,
		.node_id	= 36,
		.loop_num	= 2,
		.pri_max	= 0,
		.flow_id	= 1,
		.drr_node_id	= 8,
	},
	{
		.port		= 1,
		.flow_level	= false,
		.node_id	= 144,
		.loop_num	= 16,
		.pri_max	= 8,
		.flow_id	= 36,
		.drr_node_id	= 48,
	},
	{
		.port		= 1,
		.flow_level	= false,
		.node_id	= 272,
		.loop_num	= 4,
		.pri_max	= 4,
		.flow_id	= 36,
		.drr_node_id	= 48,
	},
	{
		.port		= 2,
		.flow_level	= true,
		.node_id	= 40,
		.loop_num	= 2,
		.pri_max	= 0,
		.flow_id	= 2,
		.drr_node_id	= 12,
	},
	{
		.port		= 2,
		.flow_level	= false,
		.node_id	= 160,
		.loop_num	= 16,
		.pri_max	= 8,
		.flow_id	= 40,
		.drr_node_id	= 64,
	},
	{
		.port		= 2,
		.flow_level	= false,
		.node_id	= 276,
		.loop_num	= 4,
		.pri_max	= 4,
		.flow_id	= 40,
		.drr_node_id	= 64,
	},
	{
		.port		= 3,
		.flow_level	= true,
		.node_id	= 44,
		.loop_num	= 2,
		.pri_max	= 0,
		.flow_id	= 3,
		.drr_node_id	= 16,
	},
	{
		.port		= 3,
		.flow_level	= false,
		.node_id	= 176,
		.loop_num	= 16,
		.pri_max	= 8,
		.flow_id	= 44,
		.drr_node_id	= 80,
	},
	{
		.port		= 3,
		.flow_level	= false,
		.node_id	= 280,
		.loop_num	= 4,
		.pri_max	= 4,
		.flow_id	= 44,
		.drr_node_id	= 80,
	},
	{
		.port		= 4,
		.flow_level	= true,
		.node_id	= 48,
		.loop_num	= 2,
		.pri_max	= 0,
		.flow_id	= 4,
		.drr_node_id	= 20,
	},
	{
		.port		= 4,
		.flow_level	= false,
		.node_id	= 192,
		.loop_num	= 16,
		.pri_max	= 8,
		.flow_id	= 48,
		.drr_node_id	= 96,
	},
	{
		.port		= 4,
		.flow_level	= false,
		.node_id	= 284,
		.loop_num	= 4,
		.pri_max	= 4,
		.flow_id	= 48,
		.drr_node_id	= 96,
	},
	{
		.port		= 5,
		.flow_level	= true,
		.node_id	= 52,
		.loop_num	= 2,
		.pri_max	= 0,
		.flow_id	= 5,
		.drr_node_id	= 24,
	},
	{
		.port		= 5,
		.flow_level	= false,
		.node_id	= 208,
		.loop_num	= 16,
		.pri_max	= 8,
		.flow_id	= 52,
		.drr_node_id	= 112,
	},
	{
		.port		= 5,
		.flow_level	= false,
		.node_id	= 288,
		.loop_num	= 4,
		.pri_max	= 4,
		.flow_id	= 52,
		.drr_node_id	= 112,
	},
	{
		.port		= 6,
		.flow_level	= true,
		.node_id	= 56,
		.loop_num	= 2,
		.pri_max	= 0,
		.flow_id	= 6,
		.drr_node_id	= 28,
	},
	{
		.port		= 6,
		.flow_level	= false,
		.node_id	= 224,
		.loop_num	= 16,
		.pri_max	= 8,
		.flow_id	= 56,
		.drr_node_id	= 128,
	},
	{
		.port		= 6,
		.flow_level	= false,
		.node_id	= 292,
		.loop_num	= 4,
		.pri_max	= 4,
		.flow_id	= 56,
		.drr_node_id	= 128,
	},
	{
		.port		= 7,
		.flow_level	= true,
		.node_id	= 60,
		.loop_num	= 2,
		.pri_max	= 0,
		.flow_id	= 7,
		.drr_node_id	= 32,
	},
	{
		.port		= 7,
		.flow_level	= false,
		.node_id	= 240,
		.loop_num	= 16,
		.pri_max	= 8,
		.flow_id	= 60,
		.drr_node_id	= 144,
	},
	{
		.port		= 7,
		.flow_level	= false,
		.node_id	= 296,
		.loop_num	= 4,
		.pri_max	= 4,
		.flow_id	= 60,
		.drr_node_id	= 144,
	},
};

/* The scheduler resource is applied to each PPE port, The resource
 * includes the unicast & multicast queues, flow nodes and DRR nodes.
 */
static const struct ppe_port_schedule_resource ipq9574_ppe_scheduler_res[] = {
	{	.ucastq_start	= 0,
		.ucastq_end	= 63,
		.mcastq_start	= 256,
		.mcastq_end	= 271,
		.flow_id_start	= 0,
		.flow_id_end	= 0,
		.l0node_start	= 0,
		.l0node_end	= 7,
		.l1node_start	= 0,
		.l1node_end	= 0,
	},
	{	.ucastq_start	= 144,
		.ucastq_end	= 159,
		.mcastq_start	= 272,
		.mcastq_end	= 275,
		.flow_id_start	= 36,
		.flow_id_end	= 39,
		.l0node_start	= 48,
		.l0node_end	= 63,
		.l1node_start	= 8,
		.l1node_end	= 11,
	},
	{	.ucastq_start	= 160,
		.ucastq_end	= 175,
		.mcastq_start	= 276,
		.mcastq_end	= 279,
		.flow_id_start	= 40,
		.flow_id_end	= 43,
		.l0node_start	= 64,
		.l0node_end	= 79,
		.l1node_start	= 12,
		.l1node_end	= 15,
	},
	{	.ucastq_start	= 176,
		.ucastq_end	= 191,
		.mcastq_start	= 280,
		.mcastq_end	= 283,
		.flow_id_start	= 44,
		.flow_id_end	= 47,
		.l0node_start	= 80,
		.l0node_end	= 95,
		.l1node_start	= 16,
		.l1node_end	= 19,
	},
	{	.ucastq_start	= 192,
		.ucastq_end	= 207,
		.mcastq_start	= 284,
		.mcastq_end	= 287,
		.flow_id_start	= 48,
		.flow_id_end	= 51,
		.l0node_start	= 96,
		.l0node_end	= 111,
		.l1node_start	= 20,
		.l1node_end	= 23,
	},
	{	.ucastq_start	= 208,
		.ucastq_end	= 223,
		.mcastq_start	= 288,
		.mcastq_end	= 291,
		.flow_id_start	= 52,
		.flow_id_end	= 55,
		.l0node_start	= 112,
		.l0node_end	= 127,
		.l1node_start	= 24,
		.l1node_end	= 27,
	},
	{	.ucastq_start	= 224,
		.ucastq_end	= 239,
		.mcastq_start	= 292,
		.mcastq_end	= 295,
		.flow_id_start	= 56,
		.flow_id_end	= 59,
		.l0node_start	= 128,
		.l0node_end	= 143,
		.l1node_start	= 28,
		.l1node_end	= 31,
	},
	{	.ucastq_start	= 240,
		.ucastq_end	= 255,
		.mcastq_start	= 296,
		.mcastq_end	= 299,
		.flow_id_start	= 60,
		.flow_id_end	= 63,
		.l0node_start	= 144,
		.l0node_end	= 159,
		.l1node_start	= 32,
		.l1node_end	= 35,
	},
	{	.ucastq_start	= 64,
		.ucastq_end	= 143,
		.mcastq_start	= 0,
		.mcastq_end	= 0,
		.flow_id_start	= 1,
		.flow_id_end	= 35,
		.l0node_start	= 8,
		.l0node_end	= 47,
		.l1node_start	= 1,
		.l1node_end	= 7,
	},
};

/* IPQ6018 has a smaller buffer pool than IPQ9574 and five physical ports.
 * BM ports 0-7 are for EDMA port 0, BM ports 8-12 are for PPE physical ports
 * 1-5, and BM ports 13-14 are internal. The pool of 1024 buffers is
 * assigned to 'group0'.
 */
static const struct ppe_bm_port_config ipq6018_ppe_bm_port_config[] = {
	{
		/* Buffer configuration for the BM port ID 0-7 of EDMA. */
		.port_id_start	= 0,
		.port_id_end	= 7,
		.pre_alloc	= 0,
		.in_fly_buf	= 100,
		.ceil		= 216,
		.weight		= 4,
		.resume_offset	= 36,
		.resume_ceil	= 0,
		.dynamic	= true,
	},
	{
		/* Buffer configuration for the BM port ID 8-12 of PPE ports. */
		.port_id_start	= 8,
		.port_id_end	= 12,
		.pre_alloc	= 0,
		.in_fly_buf	= 128,
		.ceil		= 216,
		.weight		= 4,
		.resume_offset	= 36,
		.resume_ceil	= 0,
		.dynamic	= true,
	},
	{
		/* Buffer configuration for the BM port ID 13-14 of EIP. */
		.port_id_start	= 13,
		.port_id_end	= 14,
		.pre_alloc	= 0,
		.in_fly_buf	= 40,
		.ceil		= 216,
		.weight		= 4,
		.resume_offset	= 36,
		.resume_ceil	= 0,
		.dynamic	= true,
	},
};

/* Default QM settings for unicast and multicast queues for IPQ6018. */
static const struct ppe_qm_queue_config ipq6018_ppe_qm_queue_config[] = {
	{
		/* QM settings for unicast queues 0 to 255. */
		.queue_start	= 0,
		.queue_end	= 255,
		.prealloc_buf	= 0,
		.ceil		= 216,
		.weight		= 4,
		.resume_offset	= 36,
		.dynamic	= true,
	},
	{
		/* QM settings for multicast queues 256 to 299. */
		.queue_start	= 256,
		.queue_end	= 299,
		.prealloc_buf	= 0,
		.ceil		= 216,
		.weight		= 0,
		.resume_offset	= 36,
		.dynamic	= false,
	},
};

/* PPE scheduler configuration for BM on IPQ6018. The entries never use a
 * backup port.
 */
static const struct ppe_scheduler_bm_config ipq6018_ppe_sch_bm_config[] = {
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS,  0, false, 0},
	{true, PPE_SCH_INGRESS, 5, false, 0},
	{true, PPE_SCH_EGRESS,  4, false, 0},
	{true, PPE_SCH_INGRESS, 1, false, 0},
	{true, PPE_SCH_EGRESS,  5, false, 0},
	{true, PPE_SCH_INGRESS, 6, false, 0},
	{true, PPE_SCH_EGRESS,  6, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS,  0, false, 0},
	{true, PPE_SCH_INGRESS, 7, false, 0},
	{true, PPE_SCH_EGRESS,  7, false, 0},
	{true, PPE_SCH_INGRESS, 4, false, 0},
	{true, PPE_SCH_EGRESS,  0, false, 0},
	{true, PPE_SCH_INGRESS, 6, false, 0},
	{true, PPE_SCH_EGRESS,  5, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS,  0, false, 0},
	{true, PPE_SCH_INGRESS, 2, false, 0},
	{true, PPE_SCH_EGRESS,  6, false, 0},
	{true, PPE_SCH_INGRESS, 7, false, 0},
	{true, PPE_SCH_EGRESS,  7, false, 0},
	{true, PPE_SCH_INGRESS, 5, false, 0},
	{true, PPE_SCH_EGRESS,  5, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS,  0, false, 0},
	{true, PPE_SCH_INGRESS, 6, false, 0},
	{true, PPE_SCH_EGRESS,  6, false, 0},
	{true, PPE_SCH_INGRESS, 7, false, 0},
	{true, PPE_SCH_EGRESS,  3, false, 0},
	{true, PPE_SCH_INGRESS, 5, false, 0},
	{true, PPE_SCH_EGRESS,  7, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS,  0, false, 0},
	{true, PPE_SCH_INGRESS, 6, false, 0},
	{true, PPE_SCH_EGRESS,  5, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS,  6, false, 0},
	{true, PPE_SCH_INGRESS, 7, false, 0},
	{true, PPE_SCH_EGRESS,  7, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS,  0, false, 0},
	{true, PPE_SCH_INGRESS, 5, false, 0},
	{true, PPE_SCH_EGRESS,  4, false, 0},
	{true, PPE_SCH_INGRESS, 6, false, 0},
	{true, PPE_SCH_EGRESS,  6, false, 0},
	{true, PPE_SCH_INGRESS, 7, false, 0},
	{true, PPE_SCH_EGRESS,  7, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS,  0, false, 0},
	{true, PPE_SCH_INGRESS, 4, false, 0},
	{true, PPE_SCH_EGRESS,  5, false, 0},
	{true, PPE_SCH_INGRESS, 5, false, 0},
	{true, PPE_SCH_EGRESS,  1, false, 0},
	{true, PPE_SCH_INGRESS, 6, false, 0},
	{true, PPE_SCH_EGRESS,  6, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS,  0, false, 0},
	{true, PPE_SCH_INGRESS, 7, false, 0},
	{true, PPE_SCH_EGRESS,  7, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS,  4, false, 0},
	{true, PPE_SCH_INGRESS, 5, false, 0},
	{true, PPE_SCH_EGRESS,  6, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS,  0, false, 0},
	{true, PPE_SCH_INGRESS, 6, false, 0},
	{true, PPE_SCH_EGRESS,  2, false, 0},
	{true, PPE_SCH_INGRESS, 7, false, 0},
	{true, PPE_SCH_EGRESS,  7, false, 0},
	{true, PPE_SCH_INGRESS, 5, false, 0},
	{true, PPE_SCH_EGRESS,  5, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS,  0, false, 0},
	{true, PPE_SCH_INGRESS, 6, false, 0},
	{true, PPE_SCH_EGRESS,  6, false, 0},
	{true, PPE_SCH_INGRESS, 3, false, 0},
	{true, PPE_SCH_EGRESS,  7, false, 0},
	{true, PPE_SCH_INGRESS, 7, false, 0},
	{true, PPE_SCH_EGRESS,  5, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS,  0, false, 0},
	{true, PPE_SCH_INGRESS, 5, false, 0},
	{true, PPE_SCH_EGRESS,  6, false, 0},
	{true, PPE_SCH_INGRESS, 6, false, 0},
	{true, PPE_SCH_EGRESS,  0, false, 0},
	{true, PPE_SCH_INGRESS, 7, false, 0},
	{true, PPE_SCH_EGRESS,  7, false, 0},
	{true, PPE_SCH_INGRESS, 0, false, 0},
	{true, PPE_SCH_EGRESS,  0, false, 0},
	{true, PPE_SCH_INGRESS, 4, false, 0},
	{true, PPE_SCH_EGRESS,  5, false, 0},
	{true, PPE_SCH_INGRESS, 6, false, 0},
	{true, PPE_SCH_EGRESS,  6, false, 0},
	{true, PPE_SCH_INGRESS, 7, false, 0},
	{true, PPE_SCH_EGRESS,  7, false, 0},
};

/* PPE scheduler configuration for QM on IPQ6018. The entries never use a
 * backup dequeue port.
 */
static const struct ppe_scheduler_qm_config ipq6018_ppe_sch_qm_config[] = {
	{0xB7, 0, 6, false, 0},
	{0xBE, 3, 0, false, 0},
	{0xDE, 6, 5, false, 0},
	{0xDD, 0, 1, false, 0},
	{0xBD, 5, 6, false, 0},
	{0xBE, 1, 0, false, 0},
	{0xEE, 6, 4, false, 0},
	{0xCF, 0, 5, false, 0},
	{0x9F, 4, 6, false, 0},
	{0xBE, 5, 0, false, 0},
	{0x7E, 6, 7, false, 0},
	{0x5F, 0, 5, false, 0},
	{0x9F, 7, 6, false, 0},
	{0xBE, 5, 0, false, 0},
	{0xFA, 6, 2, false, 0},
	{0xBB, 0, 6, false, 0},
	{0x9F, 2, 5, false, 0},
	{0xCF, 6, 4, false, 0},
	{0xEE, 5, 0, false, 0},
	{0xBE, 4, 6, false, 0},
	{0x3F, 0, 7, false, 0},
	{0x5F, 6, 5, false, 0},
	{0xDE, 7, 0, false, 0},
	{0xBE, 5, 6, false, 0},
	{0xB7, 0, 3, false, 0},
	{0xE7, 6, 4, false, 0},
	{0xEE, 3, 0, false, 0},
	{0xBE, 4, 6, false, 0},
	{0x9F, 0, 5, false, 0},
	{0xDD, 6, 1, false, 0},
	{0xFC, 5, 0, false, 0},
	{0xBE, 1, 6, false, 0},
	{0x9F, 0, 5, false, 0},
	{0x5F, 6, 7, false, 0},
	{0x7E, 5, 0, false, 0},
	{0xBE, 7, 6, false, 0},
	{0xAF, 0, 4, false, 0},
	{0xCF, 6, 5, false, 0},
	{0x9F, 4, 6, false, 0},
	{0xBE, 5, 0, false, 0},
	{0xFA, 6, 2, false, 0},
	{0xDB, 0, 5, false, 0},
	{0x9F, 2, 6, false, 0},
	{0xBE, 5, 0, false, 0},
	{0x7E, 6, 7, false, 0},
	{0x6F, 0, 4, false, 0},
	{0xAF, 7, 6, false, 0},
	{0x9F, 4, 5, false, 0},
	{0xDE, 6, 0, false, 0},
	{0xF6, 5, 3, false, 0},
};

const struct ppe_regs ppe_hppe_regs = {
	.mru_mtu_tbl_addr = PPE_HPPE_MRU_MTU_CTRL_TBL_ADDR,
	.vsi_tbl_addr = PPE_HPPE_VSI_TBL_ADDR,
	.vsi_tbl_entries = PPE_HPPE_VSI_TBL_ENTRIES,
	.eg_bridge_config_addr = PPE_HPPE_EG_BRIDGE_CONFIG_ADDR,
	.port_eg_vlan_tbl_addr = PPE_HPPE_PORT_EG_VLAN_TBL_ADDR,
	.l3_vp_port_tbl_addr = PPE_HPPE_L3_VP_PORT_TBL_ADDR,
	.l3_vp_port_tbl_words = PPE_HPPE_L3_VP_PORT_TBL_WORDS,
	.eg_vsi_tag_addr = PPE_HPPE_EG_VSI_TAG_ADDR,
	.eg_vsi_tag_inc = PPE_HPPE_EG_VSI_TAG_INC,
	.eg_vsi_counter_tbl_addr = PPE_HPPE_EG_VSI_COUNTER_TBL_ADDR,
	.port_tx_counter_tbl_addr = PPE_HPPE_PORT_TX_COUNTER_TBL_ADDR,
	.vport_tx_counter_tbl_addr = PPE_HPPE_VPORT_TX_COUNTER_TBL_ADDR,
	.queue_tx_counter_tbl_addr = PPE_HPPE_QUEUE_TX_COUNTER_TBL_ADDR,
};

const struct ppe_regs ppe_appe_regs = {
	.mru_mtu_tbl_addr = PPE_APPE_MRU_MTU_CTRL_TBL_ADDR,
	.vsi_tbl_addr = PPE_APPE_VSI_TBL_ADDR,
	.vsi_tbl_entries = PPE_APPE_VSI_TBL_ENTRIES,
	.eg_bridge_config_addr = PPE_APPE_EG_BRIDGE_CONFIG_ADDR,
	.port_eg_vlan_tbl_addr = PPE_APPE_PORT_EG_VLAN_TBL_ADDR,
	.l3_vp_port_tbl_addr = PPE_APPE_L3_VP_PORT_TBL_ADDR,
	.l3_vp_port_tbl_words = PPE_APPE_L3_VP_PORT_TBL_WORDS,
	.eg_vsi_tag_addr = PPE_APPE_EG_VSI_TAG_ADDR,
	.eg_vsi_tag_inc = PPE_APPE_EG_VSI_TAG_INC,
	.eg_vsi_counter_tbl_addr = PPE_APPE_EG_VSI_COUNTER_TBL_ADDR,
	.port_tx_counter_tbl_addr = PPE_APPE_PORT_TX_COUNTER_TBL_ADDR,
	.vport_tx_counter_tbl_addr = PPE_APPE_VPORT_TX_COUNTER_TBL_ADDR,
	.queue_tx_counter_tbl_addr = PPE_APPE_QUEUE_TX_COUNTER_TBL_ADDR,
};

/**
 * struct ppe_config_data - PPE BM, QM and scheduler configuration of a SoC.
 * @bm_group_buffers: Buffers assigned to BM group 0.
 * @bm_port_cfg: Buffer configuration per BM port.
 * @bm_port_cfg_num: Number of entries in @bm_port_cfg.
 * @qm_group_buffers: Buffers assigned to QM group 0.
 * @qm_queue_cfg: Threshold configuration per queue range.
 * @qm_queue_cfg_num: Number of entries in @qm_queue_cfg.
 * @sch_bm_cfg: BM scheduler entries.
 * @sch_bm_cfg_num: Number of entries in @sch_bm_cfg.
 * @sch_qm_cfg: QM scheduler entries.
 * @sch_qm_cfg_num: Number of entries in @sch_qm_cfg.
 * @sch_port_cfg: Scheduler configuration per port.
 * @sch_port_cfg_num: Number of entries in @sch_port_cfg.
 * @sch_res: Scheduler resources indexed by PPE port.
 * @mru_mtu_tbl_inc: Distance between the entries of the table.
 */
struct ppe_config_data {
	int bm_group_buffers;
	const struct ppe_bm_port_config *bm_port_cfg;
	unsigned int bm_port_cfg_num;
	int qm_group_buffers;
	const struct ppe_qm_queue_config *qm_queue_cfg;
	unsigned int qm_queue_cfg_num;
	const struct ppe_scheduler_bm_config *sch_bm_cfg;
	unsigned int sch_bm_cfg_num;
	const struct ppe_scheduler_qm_config *sch_qm_cfg;
	unsigned int sch_qm_cfg_num;
	const struct ppe_scheduler_port_config *sch_port_cfg;
	unsigned int sch_port_cfg_num;
	const struct ppe_port_schedule_resource *sch_res;
	u32 mru_mtu_tbl_inc;
};

const struct ppe_config_data ppe_ipq9574_config = {
	/* There are total 2048 buffers available in PPE, out of which some
	 * buffers are reserved for some specific purposes per PPE port. The
	 * rest of the pool of 1550 buffers are assigned to the general
	 * 'group0' which is shared among all ports of the PPE.
	 */
	.bm_group_buffers = 1550,
	.bm_port_cfg = ipq9574_ppe_bm_port_config,
	.bm_port_cfg_num = ARRAY_SIZE(ipq9574_ppe_bm_port_config),
	/* QM fetches the packet from PPE buffer management for transmitting
	 * the packet out. The QM group configuration limits the total number
	 * of buffers enqueued by all PPE hardware queues. There are total
	 * 2048 buffers available, out of which some buffers are dedicated to
	 * hardware exception handlers. The remaining buffers are assigned to
	 * the general 'group0', which is the group assigned to all queues by
	 * default.
	 */
	.qm_group_buffers = 2000,
	.qm_queue_cfg = ipq9574_ppe_qm_queue_config,
	.qm_queue_cfg_num = ARRAY_SIZE(ipq9574_ppe_qm_queue_config),
	.sch_bm_cfg = ipq9574_ppe_sch_bm_config,
	.sch_bm_cfg_num = ARRAY_SIZE(ipq9574_ppe_sch_bm_config),
	.sch_qm_cfg = ipq9574_ppe_sch_qm_config,
	.sch_qm_cfg_num = ARRAY_SIZE(ipq9574_ppe_sch_qm_config),
	.sch_port_cfg = ipq9574_ppe_port_sch_config,
	.sch_port_cfg_num = ARRAY_SIZE(ipq9574_ppe_port_sch_config),
	.sch_res = ipq9574_ppe_scheduler_res,
	.mru_mtu_tbl_inc = PPE_MRU_MTU_CTRL_TBL_INC,
};

/* The IPQ5424 has the same buffers as the IPQ9574, and the queue management
 * of the IPQ9574 applies. The buffer manager ports and the schedulers are
 * its own.
 */
const struct ppe_config_data ppe_ipq5424_config = {
	.bm_group_buffers = 1550,
	.bm_port_cfg = ipq5424_ppe_bm_port_config,
	.bm_port_cfg_num = ARRAY_SIZE(ipq5424_ppe_bm_port_config),
	.qm_group_buffers = 2000,
	.qm_queue_cfg = ipq9574_ppe_qm_queue_config,
	.qm_queue_cfg_num = ARRAY_SIZE(ipq9574_ppe_qm_queue_config),
	.sch_bm_cfg = ipq5424_ppe_sch_bm_config,
	.sch_bm_cfg_num = ARRAY_SIZE(ipq5424_ppe_sch_bm_config),
	.sch_qm_cfg = ipq5424_ppe_sch_qm_config,
	.sch_qm_cfg_num = ARRAY_SIZE(ipq5424_ppe_sch_qm_config),
	.sch_port_cfg = ipq9574_ppe_port_sch_config,
	.sch_port_cfg_num = ARRAY_SIZE(ipq9574_ppe_port_sch_config),
	.sch_res = ipq9574_ppe_scheduler_res,
	.mru_mtu_tbl_inc = PPE_MRU_MTU_CTRL_TBL_INC,
};

/* The scheduler queue and flow resources of ports 1 to 7 are shared with
 * IPQ9574. The configuration of the CPU port 0 has not been checked against
 * the IPQ6018 hardware.
 */
const struct ppe_config_data ppe_ipq6018_config = {
	.bm_group_buffers = 1024,
	.bm_port_cfg = ipq6018_ppe_bm_port_config,
	.bm_port_cfg_num = ARRAY_SIZE(ipq6018_ppe_bm_port_config),
	.qm_group_buffers = 1506,
	.qm_queue_cfg = ipq6018_ppe_qm_queue_config,
	.qm_queue_cfg_num = ARRAY_SIZE(ipq6018_ppe_qm_queue_config),
	.sch_bm_cfg = ipq6018_ppe_sch_bm_config,
	.sch_bm_cfg_num = ARRAY_SIZE(ipq6018_ppe_sch_bm_config),
	.sch_qm_cfg = ipq6018_ppe_sch_qm_config,
	.sch_qm_cfg_num = ARRAY_SIZE(ipq6018_ppe_sch_qm_config),
	.sch_port_cfg = ipq9574_ppe_port_sch_config,
	.sch_port_cfg_num = ARRAY_SIZE(ipq9574_ppe_port_sch_config),
	.sch_res = ipq9574_ppe_scheduler_res,
	.mru_mtu_tbl_inc = 0x10,
};

/* The IPQ8074 has the ports of the IPQ9574, and the scheduler resources of the
 * ports are shared with it. Its buffers, its queue management and its
 * schedulers are its own, and it places the tables of the switch like the
 * IPQ6018.
 */
const struct ppe_config_data ppe_ipq8074_config = {
	.bm_group_buffers = 1400,
	.bm_port_cfg = ipq8074_ppe_bm_port_config,
	.bm_port_cfg_num = ARRAY_SIZE(ipq8074_ppe_bm_port_config),
	.qm_group_buffers = 2000,
	.qm_queue_cfg = ipq8074_ppe_qm_queue_config,
	.qm_queue_cfg_num = ARRAY_SIZE(ipq8074_ppe_qm_queue_config),
	.sch_bm_cfg = ipq8074_ppe_sch_bm_config,
	.sch_bm_cfg_num = ARRAY_SIZE(ipq8074_ppe_sch_bm_config),
	.sch_qm_cfg = ipq8074_ppe_sch_qm_config,
	.sch_qm_cfg_num = ARRAY_SIZE(ipq8074_ppe_sch_qm_config),
	.sch_port_cfg = ipq9574_ppe_port_sch_config,
	.sch_port_cfg_num = ARRAY_SIZE(ipq9574_ppe_port_sch_config),
	.sch_res = ipq9574_ppe_scheduler_res,
	/* The entries of the MRU and MTU table have two words. */
	.mru_mtu_tbl_inc = 0x8,
};

/* Set the PPE queue level scheduler configuration. */
static int ppe_scheduler_l0_queue_map_set(struct ppe_device *ppe_dev,
					  int node_id, int port,
					  struct ppe_scheduler_cfg scheduler_cfg)
{
	u32 val, reg;
	int ret;

	reg = PPE_L0_FLOW_MAP_TBL_ADDR + node_id * PPE_L0_FLOW_MAP_TBL_INC;
	val = FIELD_PREP(PPE_L0_FLOW_MAP_TBL_FLOW_ID, scheduler_cfg.flow_id);
	val |= FIELD_PREP(PPE_L0_FLOW_MAP_TBL_C_PRI, scheduler_cfg.pri);
	val |= FIELD_PREP(PPE_L0_FLOW_MAP_TBL_E_PRI, scheduler_cfg.pri);
	val |= FIELD_PREP(PPE_L0_FLOW_MAP_TBL_C_NODE_WT, scheduler_cfg.drr_node_wt);
	val |= FIELD_PREP(PPE_L0_FLOW_MAP_TBL_E_NODE_WT, scheduler_cfg.drr_node_wt);

	ret = regmap_write(ppe_dev->regmap, reg, val);
	if (ret)
		return ret;

	reg = PPE_L0_C_FLOW_CFG_TBL_ADDR +
	      (scheduler_cfg.flow_id * PPE_QUEUE_SCH_PRI_NUM + scheduler_cfg.pri) *
	      PPE_L0_C_FLOW_CFG_TBL_INC;
	val = FIELD_PREP(PPE_L0_C_FLOW_CFG_TBL_NODE_ID, scheduler_cfg.drr_node_id);
	val |= FIELD_PREP(PPE_L0_C_FLOW_CFG_TBL_NODE_CREDIT_UNIT, scheduler_cfg.unit_is_packet);

	ret = regmap_write(ppe_dev->regmap, reg, val);
	if (ret)
		return ret;

	reg = PPE_L0_E_FLOW_CFG_TBL_ADDR +
	      (scheduler_cfg.flow_id * PPE_QUEUE_SCH_PRI_NUM + scheduler_cfg.pri) *
	      PPE_L0_E_FLOW_CFG_TBL_INC;
	val = FIELD_PREP(PPE_L0_E_FLOW_CFG_TBL_NODE_ID, scheduler_cfg.drr_node_id);
	val |= FIELD_PREP(PPE_L0_E_FLOW_CFG_TBL_NODE_CREDIT_UNIT, scheduler_cfg.unit_is_packet);

	ret = regmap_write(ppe_dev->regmap, reg, val);
	if (ret)
		return ret;

	reg = PPE_L0_FLOW_PORT_MAP_TBL_ADDR + node_id * PPE_L0_FLOW_PORT_MAP_TBL_INC;
	val = FIELD_PREP(PPE_L0_FLOW_PORT_MAP_TBL_PORT_NUM, port);

	ret = regmap_write(ppe_dev->regmap, reg, val);
	if (ret)
		return ret;

	reg = PPE_L0_COMP_CFG_TBL_ADDR + node_id * PPE_L0_COMP_CFG_TBL_INC;
	val = FIELD_PREP(PPE_L0_COMP_CFG_TBL_NODE_METER_LEN, scheduler_cfg.frame_mode);

	return regmap_update_bits(ppe_dev->regmap, reg,
				  PPE_L0_COMP_CFG_TBL_NODE_METER_LEN,
				  val);
}

/* Get the first level scheduler configuration. */
static int ppe_scheduler_l0_queue_map_get(struct ppe_device *ppe_dev,
					  int node_id, int *port,
					  struct ppe_scheduler_cfg *scheduler_cfg)
{
	u32 val, reg;
	int ret;

	reg = PPE_L0_FLOW_MAP_TBL_ADDR + node_id * PPE_L0_FLOW_MAP_TBL_INC;
	ret = regmap_read(ppe_dev->regmap, reg, &val);
	if (ret)
		return ret;

	scheduler_cfg->flow_id = FIELD_GET(PPE_L0_FLOW_MAP_TBL_FLOW_ID, val);
	scheduler_cfg->pri = FIELD_GET(PPE_L0_FLOW_MAP_TBL_C_PRI, val);
	scheduler_cfg->drr_node_wt = FIELD_GET(PPE_L0_FLOW_MAP_TBL_C_NODE_WT, val);

	reg = PPE_L0_C_FLOW_CFG_TBL_ADDR +
	      (scheduler_cfg->flow_id * PPE_QUEUE_SCH_PRI_NUM + scheduler_cfg->pri) *
	      PPE_L0_C_FLOW_CFG_TBL_INC;

	ret = regmap_read(ppe_dev->regmap, reg, &val);
	if (ret)
		return ret;

	scheduler_cfg->drr_node_id = FIELD_GET(PPE_L0_C_FLOW_CFG_TBL_NODE_ID, val);
	scheduler_cfg->unit_is_packet = FIELD_GET(PPE_L0_C_FLOW_CFG_TBL_NODE_CREDIT_UNIT, val);

	reg = PPE_L0_FLOW_PORT_MAP_TBL_ADDR + node_id * PPE_L0_FLOW_PORT_MAP_TBL_INC;
	ret = regmap_read(ppe_dev->regmap, reg, &val);
	if (ret)
		return ret;

	*port = FIELD_GET(PPE_L0_FLOW_PORT_MAP_TBL_PORT_NUM, val);

	reg = PPE_L0_COMP_CFG_TBL_ADDR + node_id * PPE_L0_COMP_CFG_TBL_INC;
	ret = regmap_read(ppe_dev->regmap, reg, &val);
	if (ret)
		return ret;

	scheduler_cfg->frame_mode = FIELD_GET(PPE_L0_COMP_CFG_TBL_NODE_METER_LEN, val);

	return 0;
}

/* Set the PPE flow level scheduler configuration. */
static int ppe_scheduler_l1_queue_map_set(struct ppe_device *ppe_dev,
					  int node_id, int port,
					  struct ppe_scheduler_cfg scheduler_cfg)
{
	u32 val, reg;
	int ret;

	val = FIELD_PREP(PPE_L1_FLOW_MAP_TBL_FLOW_ID, scheduler_cfg.flow_id);
	val |= FIELD_PREP(PPE_L1_FLOW_MAP_TBL_C_PRI, scheduler_cfg.pri);
	val |= FIELD_PREP(PPE_L1_FLOW_MAP_TBL_E_PRI, scheduler_cfg.pri);
	val |= FIELD_PREP(PPE_L1_FLOW_MAP_TBL_C_NODE_WT, scheduler_cfg.drr_node_wt);
	val |= FIELD_PREP(PPE_L1_FLOW_MAP_TBL_E_NODE_WT, scheduler_cfg.drr_node_wt);
	reg = PPE_L1_FLOW_MAP_TBL_ADDR + node_id * PPE_L1_FLOW_MAP_TBL_INC;

	ret = regmap_write(ppe_dev->regmap, reg, val);
	if (ret)
		return ret;

	val = FIELD_PREP(PPE_L1_C_FLOW_CFG_TBL_NODE_ID, scheduler_cfg.drr_node_id);
	val |= FIELD_PREP(PPE_L1_C_FLOW_CFG_TBL_NODE_CREDIT_UNIT, scheduler_cfg.unit_is_packet);
	reg = PPE_L1_C_FLOW_CFG_TBL_ADDR +
	      (scheduler_cfg.flow_id * PPE_QUEUE_SCH_PRI_NUM + scheduler_cfg.pri) *
	      PPE_L1_C_FLOW_CFG_TBL_INC;

	ret = regmap_write(ppe_dev->regmap, reg, val);
	if (ret)
		return ret;

	val = FIELD_PREP(PPE_L1_E_FLOW_CFG_TBL_NODE_ID, scheduler_cfg.drr_node_id);
	val |= FIELD_PREP(PPE_L1_E_FLOW_CFG_TBL_NODE_CREDIT_UNIT, scheduler_cfg.unit_is_packet);
	reg = PPE_L1_E_FLOW_CFG_TBL_ADDR +
		(scheduler_cfg.flow_id * PPE_QUEUE_SCH_PRI_NUM + scheduler_cfg.pri) *
		PPE_L1_E_FLOW_CFG_TBL_INC;

	ret = regmap_write(ppe_dev->regmap, reg, val);
	if (ret)
		return ret;

	val = FIELD_PREP(PPE_L1_FLOW_PORT_MAP_TBL_PORT_NUM, port);
	reg = PPE_L1_FLOW_PORT_MAP_TBL_ADDR + node_id * PPE_L1_FLOW_PORT_MAP_TBL_INC;

	ret = regmap_write(ppe_dev->regmap, reg, val);
	if (ret)
		return ret;

	reg = PPE_L1_COMP_CFG_TBL_ADDR + node_id * PPE_L1_COMP_CFG_TBL_INC;
	val = FIELD_PREP(PPE_L1_COMP_CFG_TBL_NODE_METER_LEN, scheduler_cfg.frame_mode);

	return regmap_update_bits(ppe_dev->regmap, reg, PPE_L1_COMP_CFG_TBL_NODE_METER_LEN, val);
}

/* Get the second level scheduler configuration. */
static int ppe_scheduler_l1_queue_map_get(struct ppe_device *ppe_dev,
					  int node_id, int *port,
					  struct ppe_scheduler_cfg *scheduler_cfg)
{
	u32 val, reg;
	int ret;

	reg = PPE_L1_FLOW_MAP_TBL_ADDR + node_id * PPE_L1_FLOW_MAP_TBL_INC;
	ret = regmap_read(ppe_dev->regmap, reg, &val);
	if (ret)
		return ret;

	scheduler_cfg->flow_id = FIELD_GET(PPE_L1_FLOW_MAP_TBL_FLOW_ID, val);
	scheduler_cfg->pri = FIELD_GET(PPE_L1_FLOW_MAP_TBL_C_PRI, val);
	scheduler_cfg->drr_node_wt = FIELD_GET(PPE_L1_FLOW_MAP_TBL_C_NODE_WT, val);

	reg = PPE_L1_C_FLOW_CFG_TBL_ADDR +
	      (scheduler_cfg->flow_id * PPE_QUEUE_SCH_PRI_NUM + scheduler_cfg->pri) *
	      PPE_L1_C_FLOW_CFG_TBL_INC;
	ret = regmap_read(ppe_dev->regmap, reg, &val);
	if (ret)
		return ret;

	scheduler_cfg->drr_node_id = FIELD_GET(PPE_L1_C_FLOW_CFG_TBL_NODE_ID, val);
	scheduler_cfg->unit_is_packet = FIELD_GET(PPE_L1_C_FLOW_CFG_TBL_NODE_CREDIT_UNIT, val);

	reg = PPE_L1_FLOW_PORT_MAP_TBL_ADDR + node_id * PPE_L1_FLOW_PORT_MAP_TBL_INC;
	ret = regmap_read(ppe_dev->regmap, reg, &val);
	if (ret)
		return ret;

	*port = FIELD_GET(PPE_L1_FLOW_PORT_MAP_TBL_PORT_NUM, val);

	reg = PPE_L1_COMP_CFG_TBL_ADDR + node_id * PPE_L1_COMP_CFG_TBL_INC;
	ret = regmap_read(ppe_dev->regmap, reg, &val);
	if (ret)
		return ret;

	scheduler_cfg->frame_mode = FIELD_GET(PPE_L1_COMP_CFG_TBL_NODE_METER_LEN, val);

	return 0;
}

/**
 * ppe_queue_scheduler_set - Configure scheduler for PPE hardware queue
 * @ppe_dev: PPE device
 * @node_id: PPE queue ID or flow ID
 * @flow_level: Flow level scheduler or queue level scheduler
 * @port: PPE port ID set scheduler configuration
 * @scheduler_cfg: PPE scheduler configuration
 *
 * PPE scheduler configuration supports queue level and flow level on
 * the PPE egress port.
 *
 * Return: 0 on success, negative error code on failure.
 */
int ppe_queue_scheduler_set(struct ppe_device *ppe_dev,
			    int node_id, bool flow_level, int port,
			    struct ppe_scheduler_cfg scheduler_cfg)
{
	if (flow_level)
		return ppe_scheduler_l1_queue_map_set(ppe_dev, node_id,
						      port, scheduler_cfg);

	return ppe_scheduler_l0_queue_map_set(ppe_dev, node_id,
					      port, scheduler_cfg);
}

/**
 * ppe_queue_scheduler_get - get QoS scheduler of PPE hardware queue
 * @ppe_dev: PPE device
 * @node_id: PPE node ID
 * @flow_level: Flow level scheduler or queue level scheduler
 * @port: PPE port ID to get scheduler config
 * @scheduler_cfg: QoS scheduler configuration
 *
 * The hardware QoS function is supported by PPE, the current scheduler
 * configuration can be acquired based on the queue ID of PPE port.
 *
 * Return 0 on success, negative error code on failure.
 */
int ppe_queue_scheduler_get(struct ppe_device *ppe_dev,
			    int node_id, bool flow_level, int *port,
			    struct ppe_scheduler_cfg *scheduler_cfg)
{
	if (flow_level)
		return ppe_scheduler_l1_queue_map_get(ppe_dev, node_id,
						      port, scheduler_cfg);

	return ppe_scheduler_l0_queue_map_get(ppe_dev, node_id,
					      port, scheduler_cfg);
}

/**
 * ppe_queue_priority_set - set scheduler priority of PPE hardware queue
 * @ppe_dev: PPE device
 * @node_id: PPE hardware queue ID
 * @priority: Qos scheduler priority
 *
 * Configure scheduler priority of PPE hardware queue. The queue belongs to
 * level 0 in the packet pipeline.
 *
 * Return 0 on success, negative error code on failure.
 */
int ppe_queue_priority_set(struct ppe_device *ppe_dev,
			   int node_id, int priority)
{
	struct ppe_scheduler_cfg sch_cfg;
	int ret, port, level = 0;

	ret = ppe_queue_scheduler_get(ppe_dev, node_id, level, &port, &sch_cfg);
	if (ret)
		return ret;

	sch_cfg.pri = priority;
	return ppe_queue_scheduler_set(ppe_dev, node_id, level, port, sch_cfg);
}

/**
 * ppe_queue_ucast_base_set - Set PPE unicast queue base ID and profile ID
 * @ppe_dev: PPE device
 * @queue_dst: PPE queue destination configuration
 * @queue_base: PPE queue base ID
 * @profile_id: Profile ID
 *
 * The PPE unicast queue base ID and profile ID are configured based on the
 * destination port information that can be service code or CPU code or the
 * destination port.
 *
 * Return: 0 on success, negative error code on failure.
 */
int ppe_queue_ucast_base_set(struct ppe_device *ppe_dev,
			     struct ppe_queue_ucast_dest queue_dst,
			     int queue_base, int profile_id)
{
	int index, profile_size;
	u32 val, reg;

	profile_size = queue_dst.src_profile << 8;
	if (queue_dst.service_code_en)
		index = PPE_QUEUE_BASE_SERVICE_CODE + profile_size +
			queue_dst.service_code;
	else if (queue_dst.cpu_code_en)
		index = PPE_QUEUE_BASE_CPU_CODE + profile_size +
			queue_dst.cpu_code;
	else
		index = profile_size + queue_dst.dest_port;

	val = FIELD_PREP(PPE_UCAST_QUEUE_MAP_TBL_PROFILE_ID, profile_id);
	val |= FIELD_PREP(PPE_UCAST_QUEUE_MAP_TBL_QUEUE_ID, queue_base);
	reg = PPE_UCAST_QUEUE_MAP_TBL_ADDR + index * PPE_UCAST_QUEUE_MAP_TBL_INC;

	return regmap_write(ppe_dev->regmap, reg, val);
}

/**
 * ppe_queue_ucast_offset_pri_set - Set PPE unicast queue offset based on priority
 * @ppe_dev: PPE device
 * @profile_id: Profile ID
 * @priority: PPE internal priority to be used to set queue offset
 * @queue_offset: Queue offset used for calculating the destination queue ID
 *
 * The PPE unicast queue offset is configured based on the PPE
 * internal priority.
 *
 * Return: 0 on success, negative error code on failure.
 */
int ppe_queue_ucast_offset_pri_set(struct ppe_device *ppe_dev,
				   int profile_id,
				   int priority,
				   int queue_offset)
{
	u32 val, reg;
	int index;

	index = (profile_id << 4) + priority;
	val = FIELD_PREP(PPE_UCAST_PRIORITY_MAP_TBL_CLASS, queue_offset);
	reg = PPE_UCAST_PRIORITY_MAP_TBL_ADDR + index * PPE_UCAST_PRIORITY_MAP_TBL_INC;

	return regmap_write(ppe_dev->regmap, reg, val);
}

/**
 * ppe_queue_ucast_offset_hash_set - Set PPE unicast queue offset based on hash
 * @ppe_dev: PPE device
 * @profile_id: Profile ID
 * @rss_hash: Packet hash value to be used to set queue offset
 * @queue_offset: Queue offset used for calculating the destination queue ID
 *
 * The PPE unicast queue offset is configured based on the RSS hash value.
 *
 * Return: 0 on success, negative error code on failure.
 */
int ppe_queue_ucast_offset_hash_set(struct ppe_device *ppe_dev,
				    int profile_id,
				    int rss_hash,
				    int queue_offset)
{
	u32 val, reg;
	int index;

	index = (profile_id << 8) + rss_hash;
	val = FIELD_PREP(PPE_UCAST_HASH_MAP_TBL_HASH, queue_offset);
	reg = PPE_UCAST_HASH_MAP_TBL_ADDR + index * PPE_UCAST_HASH_MAP_TBL_INC;

	return regmap_write(ppe_dev->regmap, reg, val);
}

/**
 * ppe_port_resource_get - Get PPE resource per port
 * @ppe_dev: PPE device
 * @port: PPE port
 * @type: Resource type
 * @res_start: Resource start ID returned
 * @res_end: Resource end ID returned
 *
 * PPE resource is assigned per PPE port, which is acquired for QoS scheduler.
 *
 * Return: 0 on success, negative error code on failure.
 */
int ppe_port_resource_get(struct ppe_device *ppe_dev, int port,
			  enum ppe_resource_type type,
			  int *res_start, int *res_end)
{
	struct ppe_port_schedule_resource res;

	/* The reserved resource with the maximum port ID of PPE is
	 * also allowed to be acquired.
	 */
	if (port > ppe_dev->num_ports)
		return -EINVAL;

	res = ppe_dev->data->config->sch_res[port];
	switch (type) {
	case PPE_RES_UCAST:
		*res_start = res.ucastq_start;
		*res_end = res.ucastq_end;
		break;
	case PPE_RES_MCAST:
		*res_start = res.mcastq_start;
		*res_end = res.mcastq_end;
		break;
	case PPE_RES_FLOW_ID:
		*res_start = res.flow_id_start;
		*res_end = res.flow_id_end;
		break;
	case PPE_RES_L0_NODE:
		*res_start = res.l0node_start;
		*res_end = res.l0node_end;
		break;
	case PPE_RES_L1_NODE:
		*res_start = res.l1node_start;
		*res_end = res.l1node_end;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/**
 * ppe_sc_config_set - Set PPE service code configuration
 * @ppe_dev: PPE device
 * @sc: Service ID, 0-255 supported by PPE
 * @cfg: Service code configuration
 *
 * PPE service code is used by the PPE during its packet processing stages,
 * to perform or bypass certain selected packet operations on the packet.
 *
 * Return: 0 on success, negative error code on failure.
 */
int ppe_sc_config_set(struct ppe_device *ppe_dev, int sc, struct ppe_sc_cfg cfg)
{
	u32 val, reg, servcode_val[2] = {};
	unsigned long bitmap_value;
	int ret;

	val = FIELD_PREP(PPE_IN_L2_SERVICE_TBL_DST_PORT_ID_VALID, cfg.dest_port_valid);
	val |= FIELD_PREP(PPE_IN_L2_SERVICE_TBL_DST_PORT_ID, cfg.dest_port);
	val |= FIELD_PREP(PPE_IN_L2_SERVICE_TBL_DST_DIRECTION, cfg.is_src);

	bitmap_value = bitmap_read(cfg.bitmaps.egress, 0, PPE_SC_BYPASS_EGRESS_SIZE);
	val |= FIELD_PREP(PPE_IN_L2_SERVICE_TBL_DST_BYPASS_BITMAP, bitmap_value);
	val |= FIELD_PREP(PPE_IN_L2_SERVICE_TBL_RX_CNT_EN,
			  test_bit(PPE_SC_BYPASS_COUNTER_RX, cfg.bitmaps.counter));
	val |= FIELD_PREP(PPE_IN_L2_SERVICE_TBL_TX_CNT_EN,
			  test_bit(PPE_SC_BYPASS_COUNTER_TX, cfg.bitmaps.counter));
	reg = PPE_APPE_IN_L2_SERVICE_TBL_ADDR + PPE_IN_L2_SERVICE_TBL_INC * sc;

	ret = regmap_write(ppe_dev->regmap, reg, val);
	if (ret)
		return ret;

	bitmap_value = bitmap_read(cfg.bitmaps.ingress, 0, PPE_SC_BYPASS_INGRESS_SIZE);
	PPE_SERVICE_SET_BYPASS_BITMAP(servcode_val, bitmap_value);
	PPE_SERVICE_SET_RX_CNT_EN(servcode_val,
				  test_bit(PPE_SC_BYPASS_COUNTER_RX_VLAN, cfg.bitmaps.counter));
	reg = PPE_SERVICE_TBL_ADDR + PPE_SERVICE_TBL_INC * sc;

	ret = regmap_bulk_write(ppe_dev->regmap, reg,
				servcode_val, ARRAY_SIZE(servcode_val));
	if (ret)
		return ret;

	reg = PPE_APPE_EG_SERVICE_TBL_ADDR + PPE_APPE_EG_SERVICE_TBL_INC * sc;
	ret = regmap_bulk_read(ppe_dev->regmap, reg,
			       servcode_val, ARRAY_SIZE(servcode_val));
	if (ret)
		return ret;

	PPE_EG_SERVICE_SET_NEXT_SERVCODE(servcode_val, cfg.next_service_code);
	PPE_EG_SERVICE_SET_UPDATE_ACTION(servcode_val, cfg.eip_field_update_bitmap);
	PPE_EG_SERVICE_SET_HW_SERVICE(servcode_val, cfg.eip_hw_service);
	PPE_EG_SERVICE_SET_OFFSET_SEL(servcode_val, cfg.eip_offset_sel);
	PPE_EG_SERVICE_SET_TX_CNT_EN(servcode_val,
				     test_bit(PPE_SC_BYPASS_COUNTER_TX_VLAN, cfg.bitmaps.counter));

	ret = regmap_bulk_write(ppe_dev->regmap, reg,
				servcode_val, ARRAY_SIZE(servcode_val));
	if (ret)
		return ret;

	bitmap_value = bitmap_read(cfg.bitmaps.tunnel, 0, PPE_SC_BYPASS_TUNNEL_SIZE);
	val = FIELD_PREP(PPE_TL_SERVICE_TBL_BYPASS_BITMAP, bitmap_value);
	reg = PPE_TL_SERVICE_TBL_ADDR + PPE_TL_SERVICE_TBL_INC * sc;

	return regmap_write(ppe_dev->regmap, reg, val);
}

/**
 * ppe_vsi_tbl_entries - Get the number of entries in the VSI table.
 * @ppe_dev: PPE device.
 *
 * Return: Number of entries. The tables that have one entry for each VSI
 * have the same number of entries.
 */
unsigned int ppe_vsi_tbl_entries(struct ppe_device *ppe_dev)
{
	return ppe_regs(ppe_dev)->vsi_tbl_entries;
}

/**
 * ppe_counter_enable_set - Set PPE port counter enabled
 * @ppe_dev: PPE device
 * @port: PPE port ID
 *
 * Enable PPE counters on the given port for the unicast packet, multicast
 * packet and VLAN packet received and transmitted by PPE.
 *
 * Return: 0 on success, negative error code on failure.
 */
int ppe_counter_enable_set(struct ppe_device *ppe_dev, int port)
{
	const struct ppe_config_data *cfg = ppe_dev->data->config;
	const struct ppe_regs *regs = ppe_regs(ppe_dev);
	u32 reg, mru_mtu_val[3];
	int ret;

	reg = regs->mru_mtu_tbl_addr + cfg->mru_mtu_tbl_inc * port;
	ret = regmap_bulk_read(ppe_dev->regmap, reg,
			       mru_mtu_val, ARRAY_SIZE(mru_mtu_val));
	if (ret)
		return ret;

	PPE_MRU_MTU_CTRL_SET_RX_CNT_EN(mru_mtu_val, true);
	PPE_MRU_MTU_CTRL_SET_TX_CNT_EN(mru_mtu_val, true);
	ret = regmap_bulk_write(ppe_dev->regmap, reg,
				mru_mtu_val, ARRAY_SIZE(mru_mtu_val));
	if (ret)
		return ret;

	reg = PPE_MC_MTU_CTRL_TBL_ADDR + PPE_MC_MTU_CTRL_TBL_INC * port;
	ret = regmap_set_bits(ppe_dev->regmap, reg, PPE_MC_MTU_CTRL_TBL_TX_CNT_EN);
	if (ret)
		return ret;

	reg = regs->port_eg_vlan_tbl_addr + PPE_PORT_EG_VLAN_TBL_INC * port;

	return regmap_set_bits(ppe_dev->regmap, reg, PPE_PORT_EG_VLAN_TBL_TX_COUNTING_EN);
}

static int ppe_rss_hash_ipv4_config(struct ppe_device *ppe_dev, int index,
				    struct ppe_rss_hash_cfg cfg)
{
	u32 reg, val;

	switch (index) {
	case 0:
		val = cfg.hash_sip_mix[0];
		break;
	case 1:
		val = cfg.hash_dip_mix[0];
		break;
	case 2:
		val = cfg.hash_protocol_mix;
		break;
	case 3:
		val = cfg.hash_dport_mix;
		break;
	case 4:
		val = cfg.hash_sport_mix;
		break;
	default:
		return -EINVAL;
	}

	reg = PPE_RSS_HASH_MIX_IPV4_ADDR + index * PPE_RSS_HASH_MIX_IPV4_INC;

	return regmap_update_bits(ppe_dev->regmap, reg,
				  PPE_RSS_HASH_MIX_IPV4_VAL,
				  FIELD_PREP(PPE_RSS_HASH_MIX_IPV4_VAL, val));
}

static int ppe_rss_hash_ipv6_config(struct ppe_device *ppe_dev, int index,
				    struct ppe_rss_hash_cfg cfg)
{
	u32 reg, val;

	switch (index) {
	case 0 ... 3:
		val = cfg.hash_sip_mix[index];
		break;
	case 4 ... 7:
		val = cfg.hash_dip_mix[index - 4];
		break;
	case 8:
		val = cfg.hash_protocol_mix;
		break;
	case 9:
		val = cfg.hash_dport_mix;
		break;
	case 10:
		val = cfg.hash_sport_mix;
		break;
	default:
		return -EINVAL;
	}

	reg = PPE_RSS_HASH_MIX_ADDR + index * PPE_RSS_HASH_MIX_INC;

	return regmap_update_bits(ppe_dev->regmap, reg,
				  PPE_RSS_HASH_MIX_VAL,
				  FIELD_PREP(PPE_RSS_HASH_MIX_VAL, val));
}

/**
 * ppe_rss_hash_config_set - Configure the PPE hash settings for the packet received.
 * @ppe_dev: PPE device.
 * @mode: Configure RSS hash for the packet type IPv4 and IPv6.
 * @cfg: RSS hash configuration.
 *
 * PPE RSS hash settings are configured for the packet type IPv4 and IPv6.
 *
 * Return: 0 on success, negative error code on failure.
 */
int ppe_rss_hash_config_set(struct ppe_device *ppe_dev, int mode,
			    struct ppe_rss_hash_cfg cfg)
{
	u32 val, reg;
	int i, ret;

	if (mode & PPE_RSS_HASH_MODE_IPV4) {
		val = FIELD_PREP(PPE_RSS_HASH_MASK_IPV4_HASH_MASK, cfg.hash_mask);
		val |= FIELD_PREP(PPE_RSS_HASH_MASK_IPV4_FRAGMENT, cfg.hash_fragment_mode);
		ret = regmap_write(ppe_dev->regmap, PPE_RSS_HASH_MASK_IPV4_ADDR, val);
		if (ret)
			return ret;

		val = FIELD_PREP(PPE_RSS_HASH_SEED_IPV4_VAL, cfg.hash_seed);
		ret = regmap_write(ppe_dev->regmap, PPE_RSS_HASH_SEED_IPV4_ADDR, val);
		if (ret)
			return ret;

		for (i = 0; i < PPE_RSS_HASH_MIX_IPV4_ENTRIES; i++) {
			ret = ppe_rss_hash_ipv4_config(ppe_dev, i, cfg);
			if (ret)
				return ret;
		}

		for (i = 0; i < PPE_RSS_HASH_FIN_IPV4_ENTRIES; i++) {
			val = FIELD_PREP(PPE_RSS_HASH_FIN_IPV4_INNER, cfg.hash_fin_inner[i]);
			val |= FIELD_PREP(PPE_RSS_HASH_FIN_IPV4_OUTER, cfg.hash_fin_outer[i]);
			reg = PPE_RSS_HASH_FIN_IPV4_ADDR + i * PPE_RSS_HASH_FIN_IPV4_INC;

			ret = regmap_write(ppe_dev->regmap, reg, val);
			if (ret)
				return ret;
		}
	}

	if (mode & PPE_RSS_HASH_MODE_IPV6) {
		val = FIELD_PREP(PPE_RSS_HASH_MASK_HASH_MASK, cfg.hash_mask);
		val |= FIELD_PREP(PPE_RSS_HASH_MASK_FRAGMENT, cfg.hash_fragment_mode);
		ret = regmap_write(ppe_dev->regmap, PPE_RSS_HASH_MASK_ADDR, val);
		if (ret)
			return ret;

		val = FIELD_PREP(PPE_RSS_HASH_SEED_VAL, cfg.hash_seed);
		ret = regmap_write(ppe_dev->regmap, PPE_RSS_HASH_SEED_ADDR, val);
		if (ret)
			return ret;

		for (i = 0; i < PPE_RSS_HASH_MIX_ENTRIES; i++) {
			ret = ppe_rss_hash_ipv6_config(ppe_dev, i, cfg);
			if (ret)
				return ret;
		}

		for (i = 0; i < PPE_RSS_HASH_FIN_ENTRIES; i++) {
			val = FIELD_PREP(PPE_RSS_HASH_FIN_INNER, cfg.hash_fin_inner[i]);
			val |= FIELD_PREP(PPE_RSS_HASH_FIN_OUTER, cfg.hash_fin_outer[i]);
			reg = PPE_RSS_HASH_FIN_ADDR + i * PPE_RSS_HASH_FIN_INC;

			ret = regmap_write(ppe_dev->regmap, reg, val);
			if (ret)
				return ret;
		}
	}

	return 0;
}

/**
 * ppe_rx_hash_spread - Spread the frames for the CPU port over queues
 * @ppe_dev: PPE device
 * @num_queues: Number of queues to spread over
 *
 * Add a queue offset from 0 to @num_queues - 1 to the base queue of the CPU
 * port, chosen by the RSS hash of the frame. The Ethernet DMA maps these
 * queues to its receive rings.
 *
 * Return: 0 on success, negative error code on failure.
 */
int ppe_rx_hash_spread(struct ppe_device *ppe_dev, unsigned int num_queues)
{
	int hash, ret;

	for (hash = 0; hash < PPE_QUEUE_HASH_NUM; hash++) {
		ret = ppe_queue_ucast_offset_hash_set(ppe_dev, 0, hash,
						      hash % num_queues);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * ppe_ring_queue_map_set - Set the PPE queue to Ethernet DMA ring mapping
 * @ppe_dev: PPE device
 * @ring_id: Ethernet DMA ring ID
 * @queue_map: Bit map of queue IDs to given Ethernet DMA ring
 *
 * Configure the mapping from a set of PPE queues to a given Ethernet DMA ring.
 *
 * Return: 0 on success, negative error code on failure.
 */
int ppe_ring_queue_map_set(struct ppe_device *ppe_dev, int ring_id,
			   const u32 *queue_map)
{
	u32 reg, queue_bitmap_val[PPE_RING_TO_QUEUE_BITMAP_WORD_CNT];

	memcpy(queue_bitmap_val, queue_map, sizeof(queue_bitmap_val));
	reg = PPE_RING_Q_MAP_TBL_ADDR + PPE_RING_Q_MAP_TBL_INC * ring_id;

	return regmap_bulk_write(ppe_dev->regmap, reg,
				 queue_bitmap_val,
				 ARRAY_SIZE(queue_bitmap_val));
}

static int ppe_config_bm_threshold(struct ppe_device *ppe_dev, int bm_port_id,
				   struct ppe_bm_port_config port_cfg)
{
	u32 reg, val, bm_fc_val[2];
	int ret;

	reg = PPE_BM_PORT_FC_CFG_TBL_ADDR + PPE_BM_PORT_FC_CFG_TBL_INC * bm_port_id;
	ret = regmap_bulk_read(ppe_dev->regmap, reg,
			       bm_fc_val, ARRAY_SIZE(bm_fc_val));
	if (ret)
		return ret;

	/* Configure BM flow control related threshold. */
	PPE_BM_PORT_FC_SET_WEIGHT(bm_fc_val, port_cfg.weight);
	PPE_BM_PORT_FC_SET_RESUME_OFFSET(bm_fc_val, port_cfg.resume_offset);
	PPE_BM_PORT_FC_SET_RESUME_THRESHOLD(bm_fc_val, port_cfg.resume_ceil);
	PPE_BM_PORT_FC_SET_DYNAMIC(bm_fc_val, port_cfg.dynamic);
	PPE_BM_PORT_FC_SET_REACT_LIMIT(bm_fc_val, port_cfg.in_fly_buf);
	PPE_BM_PORT_FC_SET_PRE_ALLOC(bm_fc_val, port_cfg.pre_alloc);

	/* Configure low/high bits of the ceiling for the BM port. */
	val = FIELD_GET(GENMASK(2, 0), port_cfg.ceil);
	PPE_BM_PORT_FC_SET_CEILING_LOW(bm_fc_val, val);
	val = FIELD_GET(GENMASK(10, 3), port_cfg.ceil);
	PPE_BM_PORT_FC_SET_CEILING_HIGH(bm_fc_val, val);

	ret = regmap_bulk_write(ppe_dev->regmap, reg,
				bm_fc_val, ARRAY_SIZE(bm_fc_val));
	if (ret)
		return ret;

	/* Assign the default group ID 0 to the BM port. */
	val = FIELD_PREP(PPE_BM_PORT_GROUP_ID_SHARED_GROUP_ID, 0);
	reg = PPE_BM_PORT_GROUP_ID_ADDR + PPE_BM_PORT_GROUP_ID_INC * bm_port_id;
	ret = regmap_update_bits(ppe_dev->regmap, reg,
				 PPE_BM_PORT_GROUP_ID_SHARED_GROUP_ID,
				 val);
	if (ret)
		return ret;

	/* Enable BM port flow control. */
	reg = PPE_BM_PORT_FC_MODE_ADDR + PPE_BM_PORT_FC_MODE_INC * bm_port_id;

	return regmap_set_bits(ppe_dev->regmap, reg, PPE_BM_PORT_FC_MODE_EN);
}

/* Configure the buffer threshold for the port flow control function. */
static int ppe_config_bm(struct ppe_device *ppe_dev)
{
	const struct ppe_config_data *cfg = ppe_dev->data->config;
	const struct ppe_bm_port_config *port_cfg;
	unsigned int i, bm_port_id, port_cfg_cnt;
	u32 reg, val;
	int ret;

	/* Configure the allocated buffer number only for group 0.
	 * The buffer number of group 1-3 is already cleared to 0
	 * after PPE reset during the probe of PPE driver.
	 */
	reg = PPE_BM_SHARED_GROUP_CFG_ADDR;
	val = FIELD_PREP(PPE_BM_SHARED_GROUP_CFG_SHARED_LIMIT,
			 cfg->bm_group_buffers);
	ret = regmap_update_bits(ppe_dev->regmap, reg,
				 PPE_BM_SHARED_GROUP_CFG_SHARED_LIMIT,
				 val);
	if (ret)
		goto bm_config_fail;

	/* Configure buffer thresholds for the BM ports. */
	port_cfg = cfg->bm_port_cfg;
	port_cfg_cnt = cfg->bm_port_cfg_num;
	for (i = 0; i < port_cfg_cnt; i++) {
		for (bm_port_id = port_cfg[i].port_id_start;
		     bm_port_id <= port_cfg[i].port_id_end; bm_port_id++) {
			ret = ppe_config_bm_threshold(ppe_dev, bm_port_id,
						      port_cfg[i]);
			if (ret)
				goto bm_config_fail;
		}
	}

	return 0;

bm_config_fail:
	dev_err(ppe_dev->dev, "PPE BM config error %d\n", ret);
	return ret;
}

/* Configure PPE hardware queue depth, which is decided by the threshold
 * of queue.
 */
static int ppe_config_qm(struct ppe_device *ppe_dev)
{
	const struct ppe_config_data *cfg = ppe_dev->data->config;
	const struct ppe_regs *regs = ppe_regs(ppe_dev);
	const struct ppe_qm_queue_config *queue_cfg;
	int ret, i, queue_id, queue_cfg_count;
	u32 reg, multicast_queue_cfg[5];
	u32 unicast_queue_cfg[4];
	u32 group_cfg[3];

	/* Assign the buffer number to the group 0 by default. */
	reg = PPE_AC_GRP_CFG_TBL_ADDR;
	ret = regmap_bulk_read(ppe_dev->regmap, reg,
			       group_cfg, ARRAY_SIZE(group_cfg));
	if (ret)
		goto qm_config_fail;

	PPE_AC_GRP_SET_BUF_LIMIT(group_cfg, cfg->qm_group_buffers);

	ret = regmap_bulk_write(ppe_dev->regmap, reg,
				group_cfg, ARRAY_SIZE(group_cfg));
	if (ret)
		goto qm_config_fail;

	queue_cfg = cfg->qm_queue_cfg;
	queue_cfg_count = cfg->qm_queue_cfg_num;
	for (i = 0; i < queue_cfg_count; i++) {
		queue_id = queue_cfg[i].queue_start;

		/* Configure threshold for dropping packets separately for
		 * unicast and multicast PPE queues.
		 */
		while (queue_id <= queue_cfg[i].queue_end) {
			if (queue_id < PPE_AC_UNICAST_QUEUE_CFG_TBL_ENTRIES) {
				reg = PPE_AC_UNICAST_QUEUE_CFG_TBL_ADDR +
				      PPE_AC_UNICAST_QUEUE_CFG_TBL_INC * queue_id;

				ret = regmap_bulk_read(ppe_dev->regmap, reg,
						       unicast_queue_cfg,
						       ARRAY_SIZE(unicast_queue_cfg));
				if (ret)
					goto qm_config_fail;

				PPE_AC_UNICAST_QUEUE_SET_EN(unicast_queue_cfg, true);
				PPE_AC_UNICAST_QUEUE_SET_GRP_ID(unicast_queue_cfg, 0);
				PPE_AC_UNICAST_QUEUE_SET_PRE_LIMIT(unicast_queue_cfg,
								   queue_cfg[i].prealloc_buf);
				PPE_AC_UNICAST_QUEUE_SET_DYNAMIC(unicast_queue_cfg,
								 queue_cfg[i].dynamic);
				PPE_AC_UNICAST_QUEUE_SET_WEIGHT(unicast_queue_cfg,
								queue_cfg[i].weight);
				PPE_AC_UNICAST_QUEUE_SET_THRESHOLD(unicast_queue_cfg,
								   queue_cfg[i].ceil);
				PPE_AC_UNICAST_QUEUE_SET_GRN_RESUME(unicast_queue_cfg,
								    queue_cfg[i].resume_offset);

				ret = regmap_bulk_write(ppe_dev->regmap, reg,
							unicast_queue_cfg,
							ARRAY_SIZE(unicast_queue_cfg));
				if (ret)
					goto qm_config_fail;
			} else {
				reg = PPE_AC_MULTICAST_QUEUE_CFG_TBL_ADDR +
				      PPE_AC_MULTICAST_QUEUE_CFG_TBL_INC * queue_id;

				ret = regmap_bulk_read(ppe_dev->regmap, reg,
						       multicast_queue_cfg,
						       ARRAY_SIZE(multicast_queue_cfg));
				if (ret)
					goto qm_config_fail;

				PPE_AC_MULTICAST_QUEUE_SET_EN(multicast_queue_cfg, true);
				PPE_AC_MULTICAST_QUEUE_SET_GRN_GRP_ID(multicast_queue_cfg, 0);
				PPE_AC_MULTICAST_QUEUE_SET_GRN_PRE_LIMIT(multicast_queue_cfg,
									 queue_cfg[i].prealloc_buf);
				PPE_AC_MULTICAST_QUEUE_SET_GRN_THRESHOLD(multicast_queue_cfg,
									 queue_cfg[i].ceil);
				PPE_AC_MULTICAST_QUEUE_SET_GRN_RESUME(multicast_queue_cfg,
								      queue_cfg[i].resume_offset);

				ret = regmap_bulk_write(ppe_dev->regmap, reg,
							multicast_queue_cfg,
							ARRAY_SIZE(multicast_queue_cfg));
				if (ret)
					goto qm_config_fail;
			}

			/* Enable enqueue. */
			reg = PPE_ENQ_OPR_TBL_ADDR + PPE_ENQ_OPR_TBL_INC * queue_id;
			ret = regmap_clear_bits(ppe_dev->regmap, reg,
						PPE_ENQ_OPR_TBL_ENQ_DISABLE);
			if (ret)
				goto qm_config_fail;

			/* Enable dequeue. */
			reg = PPE_DEQ_OPR_TBL_ADDR + PPE_DEQ_OPR_TBL_INC * queue_id;
			ret = regmap_clear_bits(ppe_dev->regmap, reg,
						PPE_DEQ_OPR_TBL_DEQ_DISABLE);
			if (ret)
				goto qm_config_fail;

			queue_id++;
		}
	}

	/* Enable queue counter for all PPE hardware queues. */
	ret = regmap_set_bits(ppe_dev->regmap, regs->eg_bridge_config_addr,
			      PPE_EG_BRIDGE_CONFIG_QUEUE_CNT_EN);
	if (ret)
		goto qm_config_fail;

	return 0;

qm_config_fail:
	dev_err(ppe_dev->dev, "PPE QM config error %d\n", ret);
	return ret;
}

static int ppe_node_scheduler_config(struct ppe_device *ppe_dev,
				     struct ppe_scheduler_port_config config)
{
	struct ppe_scheduler_cfg sch_cfg;
	int ret, i;

	for (i = 0; i < config.loop_num; i++) {
		if (!config.pri_max) {
			/* Round robin scheduler without priority. */
			sch_cfg.flow_id = config.flow_id;
			sch_cfg.pri = 0;
			sch_cfg.drr_node_id = config.drr_node_id;
		} else {
			sch_cfg.flow_id = config.flow_id + (i / config.pri_max);
			sch_cfg.pri = i % config.pri_max;
			sch_cfg.drr_node_id = config.drr_node_id + i;
		}

		/* Scheduler weight, must be more than 0. */
		sch_cfg.drr_node_wt = 1;
		/* Byte based to be scheduled. */
		sch_cfg.unit_is_packet = false;
		/* Frame + CRC calculated. */
		sch_cfg.frame_mode = PPE_SCH_WITH_FRAME_CRC;

		ret = ppe_queue_scheduler_set(ppe_dev, config.node_id + i,
					      config.flow_level,
					      config.port,
					      sch_cfg);
		if (ret)
			return ret;
	}

	return 0;
}

/* Initialize scheduler settings for PPE buffer utilization and dispatching
 * packet on PPE queue.
 */
static int ppe_config_scheduler(struct ppe_device *ppe_dev)
{
	const struct ppe_config_data *cfg = ppe_dev->data->config;
	const struct ppe_scheduler_port_config *port_cfg;
	const struct ppe_scheduler_qm_config *qm_cfg;
	const struct ppe_scheduler_bm_config *bm_cfg;
	int ret, i, count;
	u32 val, reg;

	count = cfg->sch_bm_cfg_num;
	bm_cfg = cfg->sch_bm_cfg;

	/* Configure the depth of BM scheduler entries. */
	val = FIELD_PREP(PPE_BM_SCH_CTRL_SCH_DEPTH, count);
	val |= FIELD_PREP(PPE_BM_SCH_CTRL_SCH_OFFSET, 0);
	val |= FIELD_PREP(PPE_BM_SCH_CTRL_SCH_EN, 1);

	ret = regmap_write(ppe_dev->regmap, PPE_BM_SCH_CTRL_ADDR, val);
	if (ret)
		goto sch_config_fail;

	/* Configure each BM scheduler entry with the valid ingress port and
	 * egress port, the second port takes effect when the specified port
	 * is in the inactive state.
	 */
	for (i = 0; i < count; i++) {
		val = FIELD_PREP(PPE_BM_SCH_CFG_TBL_VALID, bm_cfg[i].valid);
		val |= FIELD_PREP(PPE_BM_SCH_CFG_TBL_DIR, bm_cfg[i].dir);
		val |= FIELD_PREP(PPE_BM_SCH_CFG_TBL_PORT_NUM, bm_cfg[i].port);
		val |= FIELD_PREP(PPE_BM_SCH_CFG_TBL_SECOND_PORT_VALID,
				  bm_cfg[i].backup_port_valid);
		val |= FIELD_PREP(PPE_BM_SCH_CFG_TBL_SECOND_PORT,
				  bm_cfg[i].backup_port);

		reg = PPE_BM_SCH_CFG_TBL_ADDR + i * PPE_BM_SCH_CFG_TBL_INC;
		ret = regmap_write(ppe_dev->regmap, reg, val);
		if (ret)
			goto sch_config_fail;
	}

	count = cfg->sch_qm_cfg_num;
	qm_cfg = cfg->sch_qm_cfg;

	/* Configure the depth of QM scheduler entries. */
	val = FIELD_PREP(PPE_PSCH_SCH_DEPTH_CFG_SCH_DEPTH, count);
	ret = regmap_write(ppe_dev->regmap, PPE_PSCH_SCH_DEPTH_CFG_ADDR, val);
	if (ret)
		goto sch_config_fail;

	/* Configure each QM scheduler entry with enqueue port and dequeue
	 * port, the second port takes effect when the specified dequeue
	 * port is in the inactive port.
	 */
	for (i = 0; i < count; i++) {
		val = FIELD_PREP(PPE_PSCH_SCH_CFG_TBL_ENS_PORT_BITMAP,
				 qm_cfg[i].ensch_port_bmp);
		val |= FIELD_PREP(PPE_PSCH_SCH_CFG_TBL_ENS_PORT,
				  qm_cfg[i].ensch_port);
		val |= FIELD_PREP(PPE_PSCH_SCH_CFG_TBL_DES_PORT,
				  qm_cfg[i].desch_port);
		val |= FIELD_PREP(PPE_PSCH_SCH_CFG_TBL_DES_SECOND_PORT_EN,
				  qm_cfg[i].desch_backup_port_valid);
		val |= FIELD_PREP(PPE_PSCH_SCH_CFG_TBL_DES_SECOND_PORT,
				  qm_cfg[i].desch_backup_port);

		reg = PPE_PSCH_SCH_CFG_TBL_ADDR + i * PPE_PSCH_SCH_CFG_TBL_INC;
		ret = regmap_write(ppe_dev->regmap, reg, val);
		if (ret)
			goto sch_config_fail;
	}

	count = cfg->sch_port_cfg_num;
	port_cfg = cfg->sch_port_cfg;

	/* Configure scheduler per PPE queue or flow. */
	for (i = 0; i < count; i++) {
		if (port_cfg[i].port >= ppe_dev->num_ports)
			break;

		ret = ppe_node_scheduler_config(ppe_dev, port_cfg[i]);
		if (ret)
			goto sch_config_fail;
	}

	return 0;

sch_config_fail:
	dev_err(ppe_dev->dev, "PPE scheduler arbitration config error %d\n", ret);
	return ret;
};

/* Configure PPE queue destination of each PPE port. */
static int ppe_queue_dest_init(struct ppe_device *ppe_dev)
{
	int ret, port_id, index, q_base, q_offset, res_start, res_end, pri_max;
	struct ppe_queue_ucast_dest queue_dst;

	for (port_id = 0; port_id < ppe_dev->num_ports; port_id++) {
		memset(&queue_dst, 0, sizeof(queue_dst));

		ret = ppe_port_resource_get(ppe_dev, port_id, PPE_RES_UCAST,
					    &res_start, &res_end);
		if (ret)
			return ret;

		q_base = res_start;
		queue_dst.dest_port = port_id;

		/* Configure queue base ID and profile ID that is same as
		 * physical port ID.
		 */
		ret = ppe_queue_ucast_base_set(ppe_dev, queue_dst,
					       q_base, port_id);
		if (ret)
			return ret;

		/* Queue priority range supported by each PPE port */
		ret = ppe_port_resource_get(ppe_dev, port_id, PPE_RES_L0_NODE,
					    &res_start, &res_end);
		if (ret)
			return ret;

		pri_max = res_end - res_start;

		/* Redirect ARP reply packet with the max priority on CPU port,
		 * which keeps the ARP reply directed to CPU (CPU code is 101)
		 * with highest priority queue of EDMA.
		 */
		if (port_id == 0) {
			memset(&queue_dst, 0, sizeof(queue_dst));

			queue_dst.cpu_code_en = true;
			queue_dst.cpu_code = 101;
			ret = ppe_queue_ucast_base_set(ppe_dev, queue_dst,
						       q_base + pri_max,
						       0);
			if (ret)
				return ret;
		}

		/* Initialize the queue offset of internal priority. */
		for (index = 0; index < PPE_QUEUE_INTER_PRI_NUM; index++) {
			q_offset = index > pri_max ? pri_max : index;

			ret = ppe_queue_ucast_offset_pri_set(ppe_dev, port_id,
							     index, q_offset);
			if (ret)
				return ret;
		}

		/* Initialize the queue offset of RSS hash as 0 to avoid the
		 * random hardware value that will lead to the unexpected
		 * destination queue generated.
		 */
		for (index = 0; index < PPE_QUEUE_HASH_NUM; index++) {
			ret = ppe_queue_ucast_offset_hash_set(ppe_dev, port_id,
							      index, 0);
			if (ret)
				return ret;
		}
	}

	return 0;
}

/* Initialize the service code 1 used by CPU port. */
static int ppe_servcode_init(struct ppe_device *ppe_dev)
{
	struct ppe_sc_cfg sc_cfg = {};

	if (!ppe_has_cap(ppe_dev, PPE_CAP_SERVCODE))
		return 0;

	bitmap_zero(sc_cfg.bitmaps.counter, PPE_SC_BYPASS_COUNTER_SIZE);
	bitmap_zero(sc_cfg.bitmaps.tunnel, PPE_SC_BYPASS_TUNNEL_SIZE);

	bitmap_fill(sc_cfg.bitmaps.ingress, PPE_SC_BYPASS_INGRESS_SIZE);
	clear_bit(PPE_SC_BYPASS_INGRESS_FAKE_MAC_HEADER, sc_cfg.bitmaps.ingress);
	clear_bit(PPE_SC_BYPASS_INGRESS_SERVICE_CODE, sc_cfg.bitmaps.ingress);
	clear_bit(PPE_SC_BYPASS_INGRESS_FAKE_L2_PROTO, sc_cfg.bitmaps.ingress);

	bitmap_fill(sc_cfg.bitmaps.egress, PPE_SC_BYPASS_EGRESS_SIZE);
	clear_bit(PPE_SC_BYPASS_EGRESS_ACL_POST_ROUTING_CHECK, sc_cfg.bitmaps.egress);

	return ppe_sc_config_set(ppe_dev, PPE_EDMA_SC_BYPASS_ID, sc_cfg);
}

/* Initialize PPE port configurations. */
static int ppe_port_config_init(struct ppe_device *ppe_dev)
{
	const struct ppe_config_data *cfg = ppe_dev->data->config;
	const struct ppe_regs *regs = ppe_regs(ppe_dev);
	u32 reg, val, mru_mtu_val[3];
	int i, ret;

	/* MTU and MRU settings are not required for CPU port 0. */
	for (i = 1; i < ppe_dev->num_ports; i++) {
		/* Enable Ethernet port counter */
		ret = ppe_counter_enable_set(ppe_dev, i);
		if (ret)
			return ret;

		reg = regs->mru_mtu_tbl_addr + cfg->mru_mtu_tbl_inc * i;
		ret = regmap_bulk_read(ppe_dev->regmap, reg,
				       mru_mtu_val, ARRAY_SIZE(mru_mtu_val));
		if (ret)
			return ret;

		/* Drop the packet when the packet size is more than the MTU
		 * and redirect the packet to the CPU port when the received
		 * packet size is more than the MRU of the physical interface.
		 */
		PPE_MRU_MTU_CTRL_SET_MRU_CMD(mru_mtu_val, PPE_ACTION_REDIRECT_TO_CPU);
		PPE_MRU_MTU_CTRL_SET_MTU_CMD(mru_mtu_val, PPE_ACTION_DROP);
		ret = regmap_bulk_write(ppe_dev->regmap, reg,
					mru_mtu_val, ARRAY_SIZE(mru_mtu_val));
		if (ret)
			return ret;

		reg = PPE_MC_MTU_CTRL_TBL_ADDR + PPE_MC_MTU_CTRL_TBL_INC * i;
		val = FIELD_PREP(PPE_MC_MTU_CTRL_TBL_MTU_CMD, PPE_ACTION_DROP);
		ret = regmap_update_bits(ppe_dev->regmap, reg,
					 PPE_MC_MTU_CTRL_TBL_MTU_CMD,
					 val);
		if (ret)
			return ret;
	}

	/* Enable CPU port counters. */
	return ppe_counter_enable_set(ppe_dev, 0);
}

/* Initialize the PPE RSS configuration for IPv4 and IPv6 packet receive.
 * RSS settings are to calculate the random RSS hash value generated during
 * packet receive. This hash is then used to generate the queue offset used
 * to determine the queue used to transmit the packet.
 */
static int ppe_rss_hash_init(struct ppe_device *ppe_dev)
{
	u16 fins[PPE_RSS_HASH_TUPLES] = { 0x205, 0x264, 0x227, 0x245, 0x201 };
	u8 ips[PPE_RSS_HASH_IP_LENGTH] = { 0x13, 0xb, 0x13, 0xb };
	struct ppe_rss_hash_cfg hash_cfg;
	int i, ret;

	hash_cfg.hash_seed = get_random_u32();
	hash_cfg.hash_mask = 0xfff;

	/* Use 5 tuple as RSS hash key for the first fragment of TCP, UDP
	 * and UDP-Lite packets.
	 */
	hash_cfg.hash_fragment_mode = false;

	/* The final common seed configs used to calculate the RSS has value,
	 * which is available for both IPv4 and IPv6 packet.
	 */
	for (i = 0; i < ARRAY_SIZE(fins); i++) {
		hash_cfg.hash_fin_inner[i] = fins[i] & 0x1f;
		hash_cfg.hash_fin_outer[i] = fins[i] >> 5;
	}

	/* RSS seeds for IP protocol, L4 destination & source port and
	 * destination & source IP used to calculate the RSS hash value.
	 */
	hash_cfg.hash_protocol_mix = 0x13;
	hash_cfg.hash_dport_mix = 0xb;
	hash_cfg.hash_sport_mix = 0x13;
	hash_cfg.hash_dip_mix[0] = 0xb;
	hash_cfg.hash_sip_mix[0] = 0x13;

	/* Configure RSS seed configs for IPv4 packet. */
	ret = ppe_rss_hash_config_set(ppe_dev, PPE_RSS_HASH_MODE_IPV4, hash_cfg);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(ips); i++) {
		hash_cfg.hash_sip_mix[i] = ips[i];
		hash_cfg.hash_dip_mix[i] = ips[i];
	}

	/* Configure RSS seed configs for IPv6 packet. */
	return ppe_rss_hash_config_set(ppe_dev, PPE_RSS_HASH_MODE_IPV6, hash_cfg);
}

/* Initialize mapping between PPE queues assigned to CPU port 0
 * to Ethernet DMA ring 0.
 */
static int ppe_queues_to_ring_init(struct ppe_device *ppe_dev)
{
	u32 queue_bmap[PPE_RING_TO_QUEUE_BITMAP_WORD_CNT] = {};
	int ret, queue_id, queue_max;

	ret = ppe_port_resource_get(ppe_dev, 0, PPE_RES_UCAST,
				    &queue_id, &queue_max);
	if (ret)
		return ret;

	for (; queue_id <= queue_max; queue_id++)
		queue_bmap[queue_id / 32] |= BIT_MASK(queue_id % 32);

	return ppe_ring_queue_map_set(ppe_dev, 0, queue_bmap);
}

/* Initialize PPE bridge settings to only enable L2 frame receive and
 * transmit between CPU port and PPE Ethernet ports.
 */
static int ppe_bridge_init(struct ppe_device *ppe_dev)
{
	const struct ppe_regs *regs = ppe_regs(ppe_dev);
	u32 reg, mask, port_cfg[4], vsi_cfg[2];
	int ret, i;

	/* Configure the following settings for CPU port0:
	 * a.) Enable Bridge TX
	 * b.) Disable FDB new address learning
	 * c.) Disable station move address learning
	 */
	mask = PPE_PORT_BRIDGE_TXMAC_EN;
	mask |= PPE_PORT_BRIDGE_NEW_LRN_EN;
	mask |= PPE_PORT_BRIDGE_STA_MOVE_LRN_EN;
	ret = regmap_update_bits(ppe_dev->regmap,
				 PPE_PORT_BRIDGE_CTRL_ADDR,
				 mask,
				 PPE_PORT_BRIDGE_TXMAC_EN);
	if (ret)
		return ret;

	for (i = 1; i < ppe_dev->num_ports; i++) {
		/* Enable invalid VSI forwarding for all the physical ports
		 * to CPU port0, in case no VSI is assigned to the physical
		 * port.
		 */
		reg = PPE_L2_VP_PORT_TBL_ADDR + PPE_L2_VP_PORT_TBL_INC * i;
		ret = regmap_bulk_read(ppe_dev->regmap, reg,
				       port_cfg, ARRAY_SIZE(port_cfg));

		if (ret)
			return ret;

		PPE_L2_PORT_SET_INVALID_VSI_FWD_EN(port_cfg, true);
		PPE_L2_PORT_SET_DST_INFO(port_cfg, 0);

		ret = regmap_bulk_write(ppe_dev->regmap, reg,
					port_cfg, ARRAY_SIZE(port_cfg));
		if (ret)
			return ret;
	}

	for (i = 0; i < regs->vsi_tbl_entries; i++) {
		/* Set the VSI forward membership to include only CPU port0.
		 * FDB learning and forwarding take place only after switchdev
		 * is supported later to create the VSI and join the physical
		 * ports to the VSI port member.
		 */
		reg = regs->vsi_tbl_addr + PPE_VSI_TBL_INC * i;
		ret = regmap_bulk_read(ppe_dev->regmap, reg,
				       vsi_cfg, ARRAY_SIZE(vsi_cfg));
		if (ret)
			return ret;

		PPE_VSI_SET_MEMBER_PORT_BITMAP(vsi_cfg, BIT(0));
		PPE_VSI_SET_UUC_BITMAP(vsi_cfg, BIT(0));
		PPE_VSI_SET_UMC_BITMAP(vsi_cfg, BIT(0));
		PPE_VSI_SET_BC_BITMAP(vsi_cfg, BIT(0));
		PPE_VSI_SET_NEW_ADDR_LRN_EN(vsi_cfg, true);
		PPE_VSI_SET_NEW_ADDR_FWD_CMD(vsi_cfg, PPE_ACTION_FORWARD);
		PPE_VSI_SET_STATION_MOVE_LRN_EN(vsi_cfg, true);
		PPE_VSI_SET_STATION_MOVE_FWD_CMD(vsi_cfg, PPE_ACTION_FORWARD);

		ret = regmap_bulk_write(ppe_dev->regmap, reg,
					vsi_cfg, ARRAY_SIZE(vsi_cfg));
		if (ret)
			return ret;
	}

	return 0;
}

int ppe_hw_config(struct ppe_device *ppe_dev)
{
	int ret;

	ret = ppe_config_bm(ppe_dev);
	if (ret)
		return ret;

	ret = ppe_config_qm(ppe_dev);
	if (ret)
		return ret;

	ret = ppe_config_scheduler(ppe_dev);
	if (ret)
		return ret;

	ret = ppe_queue_dest_init(ppe_dev);
	if (ret)
		return ret;

	ret = ppe_servcode_init(ppe_dev);
	if (ret)
		return ret;

	ret = ppe_port_config_init(ppe_dev);
	if (ret)
		return ret;

	ret = ppe_rss_hash_init(ppe_dev);
	if (ret)
		return ret;

	ret = ppe_queues_to_ring_init(ppe_dev);
	if (ret)
		return ret;

	return ppe_bridge_init(ppe_dev);
}

/**
 * ppe_vsi_alloc - Allocate a VSI.
 * @ppe_dev: PPE device.
 *
 * The VSI starts with no member port.
 *
 * Return: the VSI id, or a negative error code.
 */
int ppe_vsi_alloc(struct ppe_device *ppe_dev)
{
	const struct ppe_regs *regs = ppe_regs(ppe_dev);
	u32 reg, val[2] = { 0, PPE_VSI_W1_NEW_ADDR_LRN_EN |
			       PPE_VSI_W1_STATION_MOVE_LRN_EN };
	unsigned int vsi;
	int ret;

	vsi = find_first_zero_bit(ppe_dev->vsi_bitmap, regs->vsi_tbl_entries);
	if (vsi >= regs->vsi_tbl_entries)
		return -ENOSPC;

	reg = regs->vsi_tbl_addr + vsi * PPE_VSI_TBL_INC;
	ret = regmap_bulk_write(ppe_dev->regmap, reg, val, ARRAY_SIZE(val));
	if (ret)
		return ret;

	set_bit(vsi, ppe_dev->vsi_bitmap);

	return vsi;
}

/**
 * ppe_vsi_free - Release a VSI.
 * @ppe_dev: PPE device.
 * @vsi: VSI id from ppe_vsi_alloc().
 */
void ppe_vsi_free(struct ppe_device *ppe_dev, u32 vsi)
{
	const struct ppe_regs *regs = ppe_regs(ppe_dev);
	u32 reg = regs->vsi_tbl_addr + vsi * PPE_VSI_TBL_INC;
	u32 val[2] = {};

	regmap_bulk_write(ppe_dev->regmap, reg, val, ARRAY_SIZE(val));
	clear_bit(vsi, ppe_dev->vsi_bitmap);
}

/**
 * ppe_vsi_reserve - Mark a VSI as in use.
 * @ppe_dev: PPE device.
 * @vsi: VSI id.
 */
void ppe_vsi_reserve(struct ppe_device *ppe_dev, u32 vsi)
{
	set_bit(vsi, ppe_dev->vsi_bitmap);
}

/**
 * ppe_vsi_set - Set the ports of a VSI.
 * @ppe_dev: PPE device.
 * @vsi: VSI id.
 * @member: Bitmap of the member ports.
 * @uuc: Bitmap of the ports that receive the unknown unicast frames.
 * @umc: Bitmap of the ports that receive the unknown multicast frames.
 * @bc: Bitmap of the ports that receive the broadcast frames.
 *
 * Return: 0 on success, negative error code on failure.
 */
int ppe_vsi_set(struct ppe_device *ppe_dev, u32 vsi, u32 member, u32 uuc,
		u32 umc, u32 bc)
{
	const struct ppe_regs *regs = ppe_regs(ppe_dev);
	u32 reg = regs->vsi_tbl_addr + vsi * PPE_VSI_TBL_INC;
	u32 val[2];

	val[0] = FIELD_PREP(PPE_VSI_W0_MEMBER_PORT_BITMAP, member) |
		 FIELD_PREP(PPE_VSI_W0_UUC_BITMAP, uuc) |
		 FIELD_PREP(PPE_VSI_W0_UMC_BITMAP, umc) |
		 FIELD_PREP(PPE_VSI_W0_BC_BITMAP, bc);
	val[1] = PPE_VSI_W1_NEW_ADDR_LRN_EN | PPE_VSI_W1_STATION_MOVE_LRN_EN;

	return regmap_bulk_write(ppe_dev->regmap, reg, val, ARRAY_SIZE(val));
}

/**
 * ppe_vsi_member_set - Set the member ports of a VSI.
 * @ppe_dev: PPE device.
 * @vsi: VSI id.
 * @portmask: Bitmap of the member ports. The same ports receive the
 *	unknown unicast, the unknown multicast and the broadcast frames.
 *
 * Return: 0 on success, negative error code on failure.
 */
int ppe_vsi_member_set(struct ppe_device *ppe_dev, u32 vsi, u32 portmask)
{
	return ppe_vsi_set(ppe_dev, vsi, portmask, portmask, portmask, portmask);
}

/**
 * ppe_port_vsi_set - Set the default VSI of a port.
 * @ppe_dev: PPE device.
 * @port: PPE port.
 * @vsi: VSI id, or PPE_VSI_INVALID to remove the assignment.
 *
 * The entry takes effect when its last word is written. The whole entry is
 * read and written back, because a write of the second word alone is not
 * applied.
 *
 * Return: 0 on success, negative error code on failure.
 */
int ppe_port_vsi_set(struct ppe_device *ppe_dev, int port, u32 vsi)
{
	const struct ppe_regs *regs = ppe_regs(ppe_dev);
	u32 reg = regs->l3_vp_port_tbl_addr + port * PPE_L3_VP_PORT_TBL_INC;
	u32 vsi_mask = GENMASK(9 + ilog2(regs->vsi_tbl_entries), 10);
	u32 val[PPE_L3_VP_PORT_TBL_WORDS_MAX];
	int ret;

	ret = regmap_bulk_read(ppe_dev->regmap, reg, val,
			       regs->l3_vp_port_tbl_words);
	if (ret)
		return ret;

	val[1] &= ~(PPE_L3_VP_PORT_W1_VSI_VALID | vsi_mask);
	if (vsi != PPE_VSI_INVALID)
		val[1] |= PPE_L3_VP_PORT_W1_VSI_VALID | field_prep(vsi_mask, vsi);

	return regmap_bulk_write(ppe_dev->regmap, reg, val,
				 regs->l3_vp_port_tbl_words);
}

/* The command ids run from 1, because a result register that has posted
 * nothing reads 0. Each result register has its own counter, so the id that
 * an operation waits for is never the one that its register already holds.
 */
static u32 ppe_fdb_next_cmd_id(u32 *counter)
{
	*counter = (*counter % PPE_FDB_OP_CMD_ID) + 1;

	return *counter;
}

/* The result register holds the id of the last command that finished. An
 * operation waits for its own id before it takes what the engine wrote.
 */
static int ppe_fdb_wait(struct ppe_device *ppe_dev, u32 rslt_reg, u32 cmd_id)
{
	u32 val;

	return regmap_read_poll_timeout_atomic(ppe_dev->regmap, rslt_reg, val,
					       FIELD_GET(PPE_FDB_RSLT_CMD_ID, val) == cmd_id,
					       1, PPE_FDB_OP_TIMEOUT_US);
}

static int ppe_fdb_write_op(struct ppe_device *ppe_dev, u32 type,
			    u32 hash_block, const u32 *data)
{
	u32 cmd_id;
	int ret;

	spin_lock_bh(&ppe_dev->fdb_lock);

	if (data) {
		ret = regmap_bulk_write(ppe_dev->regmap, PPE_FDB_OP_DATA_ADDR,
					data, PPE_FDB_DATA_WORDS);
		if (ret)
			goto out;
	}

	cmd_id = ppe_fdb_next_cmd_id(&ppe_dev->fdb_cmd_id);
	ret = regmap_write(ppe_dev->regmap, PPE_FDB_OP_ADDR,
			   FIELD_PREP(PPE_FDB_OP_CMD_ID, cmd_id) |
			   FIELD_PREP(PPE_FDB_OP_TYPE, type) |
			   FIELD_PREP(PPE_FDB_OP_HASH_BLOCK, hash_block));
	if (ret)
		goto out;

	ret = ppe_fdb_wait(ppe_dev, PPE_FDB_OP_RSLT_ADDR, cmd_id);
out:
	spin_unlock_bh(&ppe_dev->fdb_lock);

	return ret;
}

static int ppe_fdb_read_op(struct ppe_device *ppe_dev, u32 op_bits,
			   const u32 *key, u32 *data)
{
	u32 cmd_id;
	int ret;

	spin_lock_bh(&ppe_dev->fdb_lock);

	ret = regmap_bulk_write(ppe_dev->regmap, PPE_FDB_RD_OP_DATA_ADDR, key,
				PPE_FDB_DATA_WORDS);
	if (ret)
		goto out;

	cmd_id = ppe_fdb_next_cmd_id(&ppe_dev->fdb_rd_cmd_id);
	ret = regmap_write(ppe_dev->regmap, PPE_FDB_RD_OP_ADDR,
			   op_bits |
			   FIELD_PREP(PPE_FDB_OP_CMD_ID, cmd_id) |
			   FIELD_PREP(PPE_FDB_OP_TYPE, PPE_FDB_OP_TYPE_GET) |
			   FIELD_PREP(PPE_FDB_OP_HASH_BLOCK,
				      PPE_FDB_HASH_BLOCK_ALL));
	if (ret)
		goto out;

	ret = ppe_fdb_wait(ppe_dev, PPE_FDB_RD_OP_RSLT_ADDR, cmd_id);
	if (ret)
		goto out;

	ret = regmap_bulk_read(ppe_dev->regmap, PPE_FDB_RD_RSLT_DATA_ADDR, data,
			       PPE_FDB_DATA_WORDS);
out:
	spin_unlock_bh(&ppe_dev->fdb_lock);

	return ret;
}

/* The address is split over the first two words. The second word also has
 * the VSI.
 */
static void ppe_fdb_key(const unsigned char *addr, u32 vsi, u32 *data)
{
	data[0] = get_unaligned_be32(addr + 2);
	data[1] = get_unaligned_be16(addr) |
		  FIELD_PREP(PPE_FDB_DATA1_VSI, vsi);
	data[2] = 0;
}

static void ppe_fdb_encode(const unsigned char *addr, u32 vsi, u32 dst,
			   u32 dst_type, u32 age, u32 *data)
{
	ppe_fdb_key(addr, vsi, data);

	data[1] |= PPE_FDB_DATA1_VALID | PPE_FDB_DATA1_LKP_VALID |
		   FIELD_PREP(PPE_FDB_DATA1_DST_LO, dst);
	data[2] = FIELD_PREP(PPE_FDB_DATA2_DST_HI, dst >> PPE_FDB_DST_LO_BITS) |
		  FIELD_PREP(PPE_FDB_DATA2_DST_TYPE, dst_type) |
		  FIELD_PREP(PPE_FDB_DATA2_HIT_AGE, age);
}

static u32 ppe_fdb_dst(const u32 *data)
{
	return FIELD_GET(PPE_FDB_DATA1_DST_LO, data[1]) |
	       (FIELD_GET(PPE_FDB_DATA2_DST_HI, data[2]) << PPE_FDB_DST_LO_BITS);
}

/**
 * ppe_fdb_add - Add a unicast FDB entry.
 * @ppe_dev: PPE device.
 * @addr: MAC address.
 * @port: Destination port.
 * @vsi: VSI of the entry.
 *
 * The entry does not age.
 *
 * Return: 0 on success, negative error code on failure.
 */
int ppe_fdb_add(struct ppe_device *ppe_dev, const unsigned char *addr,
		int port, u32 vsi)
{
	u32 data[PPE_FDB_DATA_WORDS];

	ppe_fdb_encode(addr, vsi, port, PPE_FDB_DST_PORT, PPE_FDB_AGE_STATIC,
		       data);

	return ppe_fdb_write_op(ppe_dev, PPE_FDB_OP_TYPE_ADD,
				PPE_FDB_HASH_BLOCK_ALL, data);
}

/**
 * ppe_fdb_del - Delete a unicast FDB entry.
 * @ppe_dev: PPE device.
 * @addr: MAC address.
 * @port: Destination port of the entry.
 * @vsi: VSI of the entry.
 *
 * Return: 0 on success, negative error code on failure.
 */
int ppe_fdb_del(struct ppe_device *ppe_dev, const unsigned char *addr,
		int port, u32 vsi)
{
	u32 data[PPE_FDB_DATA_WORDS];

	ppe_fdb_encode(addr, vsi, port, PPE_FDB_DST_PORT, PPE_FDB_AGE_DYNAMIC,
		       data);

	return ppe_fdb_write_op(ppe_dev, PPE_FDB_OP_TYPE_DEL,
				PPE_FDB_HASH_BLOCK_ALL, data);
}

/**
 * ppe_fdb_read_entry - Read one slot of the FDB table.
 * @ppe_dev: PPE device.
 * @index: Slot number, from 0 to PPE_FDB_TBL_NUM - 1.
 * @addr: Filled with the MAC address.
 * @vsi: Filled with the VSI.
 * @port: Filled with the destination port.
 * @is_static: Filled with whether the entry does not age.
 *
 * Return: 0 on success, -ENOENT if the slot has no unicast entry, or another
 * negative error code on failure.
 */
int ppe_fdb_read_entry(struct ppe_device *ppe_dev, u32 index,
		       unsigned char *addr, u32 *vsi, int *port,
		       bool *is_static)
{
	u32 key[PPE_FDB_DATA_WORDS] = {};
	u32 data[PPE_FDB_DATA_WORDS];
	int ret;

	ret = ppe_fdb_read_op(ppe_dev,
			      PPE_FDB_OP_MODE |
			      FIELD_PREP(PPE_FDB_OP_ENTRY_IDX, index),
			      key, data);
	if (ret)
		return ret;

	if (!(data[1] & PPE_FDB_DATA1_VALID) ||
	    FIELD_GET(PPE_FDB_DATA2_DST_TYPE, data[2]) != PPE_FDB_DST_PORT)
		return -ENOENT;

	put_unaligned_be32(data[0], addr + 2);
	put_unaligned_be16(data[1], addr);

	*vsi = FIELD_GET(PPE_FDB_DATA1_VSI, data[1]);
	*port = ppe_fdb_dst(data);
	*is_static = FIELD_GET(PPE_FDB_DATA2_HIT_AGE, data[2]) ==
		     PPE_FDB_AGE_STATIC;

	return 0;
}

/**
 * ppe_fdb_flush - Flush the FDB table.
 * @ppe_dev: PPE device.
 *
 * Return: 0 on success, negative error code on failure.
 */
int ppe_fdb_flush(struct ppe_device *ppe_dev)
{
	return ppe_fdb_write_op(ppe_dev, PPE_FDB_OP_TYPE_FLUSH, 0, NULL);
}

/**
 * ppe_fdb_mcast_lookup - Look up the ports of a multicast FDB entry.
 * @ppe_dev: PPE device.
 * @addr: MAC address.
 * @vsi: VSI of the entry.
 * @portmap: Filled with the bitmap of the destination ports.
 *
 * Return: 0 on success, -ENOENT if there is no multicast entry, or another
 * negative error code on failure.
 */
int ppe_fdb_mcast_lookup(struct ppe_device *ppe_dev, const unsigned char *addr,
			 u32 vsi, u32 *portmap)
{
	u32 key[PPE_FDB_DATA_WORDS];
	u32 data[PPE_FDB_DATA_WORDS];
	int ret;

	ppe_fdb_key(addr, vsi, key);

	ret = ppe_fdb_read_op(ppe_dev, 0, key, data);
	if (ret)
		return ret;

	/* The destination of a unicast entry is a port number. Taken as a
	 * bitmap it would name ports that the group does not have.
	 */
	if (!(data[1] & PPE_FDB_DATA1_VALID) ||
	    FIELD_GET(PPE_FDB_DATA2_DST_TYPE, data[2]) != PPE_FDB_DST_PORTMAP)
		return -ENOENT;

	*portmap = ppe_fdb_dst(data);

	return 0;
}

/**
 * ppe_fdb_mcast_add - Add a multicast FDB entry.
 * @ppe_dev: PPE device.
 * @addr: MAC address.
 * @portmap: Bitmap of the destination ports.
 * @vsi: VSI of the entry.
 *
 * The entry does not age.
 *
 * Return: 0 on success, negative error code on failure.
 */
int ppe_fdb_mcast_add(struct ppe_device *ppe_dev, const unsigned char *addr,
		      u32 portmap, u32 vsi)
{
	u32 data[PPE_FDB_DATA_WORDS];

	ppe_fdb_encode(addr, vsi, portmap, PPE_FDB_DST_PORTMAP,
		       PPE_FDB_AGE_STATIC, data);

	return ppe_fdb_write_op(ppe_dev, PPE_FDB_OP_TYPE_ADD,
				PPE_FDB_HASH_BLOCK_ALL, data);
}

/**
 * ppe_fdb_mcast_del - Delete a multicast FDB entry.
 * @ppe_dev: PPE device.
 * @addr: MAC address.
 * @portmap: Bitmap of the destination ports of the entry.
 * @vsi: VSI of the entry.
 *
 * Return: 0 on success, negative error code on failure.
 */
int ppe_fdb_mcast_del(struct ppe_device *ppe_dev, const unsigned char *addr,
		      u32 portmap, u32 vsi)
{
	u32 data[PPE_FDB_DATA_WORDS];

	ppe_fdb_encode(addr, vsi, portmap, PPE_FDB_DST_PORTMAP,
		       PPE_FDB_AGE_STATIC, data);

	return ppe_fdb_write_op(ppe_dev, PPE_FDB_OP_TYPE_DEL,
				PPE_FDB_HASH_BLOCK_ALL, data);
}

/**
 * ppe_port_txmac_set - Let the fabric feed the MAC of a port or stop it.
 * @ppe_dev: PPE device.
 * @port: PPE port.
 * @enable: Feed the MAC when true.
 *
 * The gate is closed before a MAC is stopped, and opened after it started.
 *
 * Return: 0 on success, negative error code on failure.
 */
int ppe_port_txmac_set(struct ppe_device *ppe_dev, int port, bool enable)
{
	return regmap_update_bits(ppe_dev->regmap,
				  PPE_PORT_BRIDGE_CTRL_ADDR +
				  port * PPE_PORT_BRIDGE_CTRL_INC,
				  PPE_PORT_BRIDGE_TXMAC_EN,
				  enable ? PPE_PORT_BRIDGE_TXMAC_EN : 0);
}

/**
 * ppe_port_mtu_set - Set the frame size limit of a port.
 * @ppe_dev: PPE device.
 * @port: PPE port.
 * @frame_size: Largest frame that the port receives and transmits.
 *
 * A frame that is too large on receive goes to the CPU. On transmit it is
 * dropped. The first word of the entry has the sizes and the entry takes
 * effect when its second word is written, so the second word is written
 * back unchanged.
 *
 * Return: 0 on success, negative error code on failure.
 */
int ppe_port_mtu_set(struct ppe_device *ppe_dev, int port, u32 frame_size)
{
	const struct ppe_config_data *cfg = ppe_dev->data->config;
	const struct ppe_regs *regs = ppe_regs(ppe_dev);
	u32 reg = regs->mru_mtu_tbl_addr + port * cfg->mru_mtu_tbl_inc;
	u32 val[2];
	int ret;

	ret = regmap_read(ppe_dev->regmap, reg + sizeof(u32), &val[1]);
	if (ret)
		return ret;

	val[0] = FIELD_PREP(PPE_MRU_MTU_CTRL_W0_MRU, frame_size) |
		 FIELD_PREP(PPE_MRU_MTU_CTRL_W0_MRU_CMD,
			    PPE_ACTION_REDIRECT_TO_CPU) |
		 FIELD_PREP(PPE_MRU_MTU_CTRL_W0_MTU, frame_size) |
		 FIELD_PREP(PPE_MRU_MTU_CTRL_W0_MTU_CMD, PPE_ACTION_DROP);
	ret = regmap_bulk_write(ppe_dev->regmap, reg, val, ARRAY_SIZE(val));
	if (ret)
		return ret;

	return regmap_update_bits(ppe_dev->regmap,
				  PPE_MC_MTU_CTRL_TBL_ADDR +
				  port * PPE_MC_MTU_CTRL_TBL_INC,
				  PPE_MC_MTU_CTRL_TBL_MTU |
				  PPE_MC_MTU_CTRL_TBL_MTU_CMD,
				  FIELD_PREP(PPE_MC_MTU_CTRL_TBL_MTU, frame_size) |
				  FIELD_PREP(PPE_MC_MTU_CTRL_TBL_MTU_CMD,
					     PPE_ACTION_DROP));
}

/**
 * ppe_l2_egress_init - Enable the egress L2 editing.
 * @ppe_dev: PPE device.
 * @user_ports: Bitmap of the ports that keep the tags of their frames as
 *	they are. The other ports do not touch the tags.
 *
 * Without this the egress L2 editing is off and the tag mode of each VSI has
 * its reset value.
 *
 * Return: 0 on success, negative error code on failure.
 */
int ppe_l2_egress_init(struct ppe_device *ppe_dev, u32 user_ports)
{
	const struct ppe_regs *regs = ppe_regs(ppe_dev);
	unsigned int i;
	int ret;

	for (i = 0; i < ppe_dev->num_ports; i++) {
		u32 mode = user_ports & BIT(i) ? PPE_EG_UNMODIFIED :
						 PPE_EG_UNTOUCHED;

		ret = regmap_update_bits(ppe_dev->regmap,
					 regs->port_eg_vlan_tbl_addr +
					 i * PPE_PORT_EG_VLAN_TBL_INC,
					 PPE_PORT_EG_VLAN_TBL_CTAG_MODE |
					 PPE_PORT_EG_VLAN_TBL_STAG_MODE,
					 FIELD_PREP(PPE_PORT_EG_VLAN_TBL_CTAG_MODE, mode) |
					 FIELD_PREP(PPE_PORT_EG_VLAN_TBL_STAG_MODE, mode));
		if (ret)
			return ret;
	}

	for (i = 0; i < regs->vsi_tbl_entries; i++) {
		ret = regmap_write(ppe_dev->regmap,
				   regs->eg_vsi_tag_addr + i * regs->eg_vsi_tag_inc,
				   PPE_EG_VSI_TAG_UNMODIFIED);
		if (ret)
			return ret;
	}

	return regmap_set_bits(ppe_dev->regmap, regs->eg_bridge_config_addr,
			       PPE_EG_L2_EDIT_EN);
}

/**
 * ppe_port_fabric_setup - Set up a port of the legacy switch fabric.
 * @ppe_dev: PPE device.
 * @port: PPE port.
 * @frame_size: Largest frame that the port accepts and sends.
 * @port_mask: Ports that the port may forward to.
 *
 * The spanning tree state is forwarding, the port learns addresses, and
 * the port counters are enabled.
 *
 * Return: 0 on success, negative error code on failure.
 */
int ppe_port_fabric_setup(struct ppe_device *ppe_dev, int port, u32 frame_size,
			  u32 port_mask)
{
	u32 val;
	int ret;

	ret = regmap_write(ppe_dev->regmap,
			   PPE_CST_STATE_ADDR + port * PPE_CST_STATE_INC,
			   PPE_STP_FORWARDING);
	if (ret)
		return ret;

	ret = ppe_port_mtu_set(ppe_dev, port, frame_size);
	if (ret)
		return ret;

	val = PPE_PORT_BRIDGE_NEW_LRN_EN | PPE_PORT_BRIDGE_STA_MOVE_LRN_EN |
	      FIELD_PREP(PPE_PORT_BRIDGE_CTRL_PORT_ISOL, port_mask);
	ret = regmap_update_bits(ppe_dev->regmap,
				 PPE_PORT_BRIDGE_CTRL_ADDR +
				 port * PPE_PORT_BRIDGE_CTRL_INC,
				 PPE_PORT_BRIDGE_NEW_LRN_EN |
				 PPE_PORT_BRIDGE_STA_MOVE_LRN_EN |
				 PPE_PORT_BRIDGE_CTRL_PORT_ISOL,
				 val);
	if (ret)
		return ret;

	return ppe_counter_enable_set(ppe_dev, port);
}

/**
 * ppe_user_port_setup - Give a user port its default VSI.
 * @ppe_dev: PPE device.
 * @port: PPE port.
 * @vsi: Default VSI of the port.
 *
 * The L2 VP port table entry of a user port must have the port itself as its
 * destination. With the CPU as the destination, the frames that the CPU
 * sends to the port would go back to the CPU.
 *
 * Return: 0 on success, negative error code on failure.
 */
int ppe_user_port_setup(struct ppe_device *ppe_dev, int port, u32 vsi)
{
	u32 reg = PPE_L2_VP_PORT_TBL_ADDR + PPE_L2_VP_PORT_TBL_INC * port;
	u32 port_cfg[4];
	int ret;

	ret = ppe_port_vsi_set(ppe_dev, port, vsi);
	if (ret)
		return ret;

	ret = regmap_bulk_read(ppe_dev->regmap, reg, port_cfg,
			       ARRAY_SIZE(port_cfg));
	if (ret)
		return ret;

	PPE_L2_PORT_SET_INVALID_VSI_FWD_EN(port_cfg, false);
	PPE_L2_PORT_SET_DST_INFO(port_cfg, port);

	return regmap_bulk_write(ppe_dev->regmap, reg, port_cfg,
				 ARRAY_SIZE(port_cfg));
}

/**
 * ppe_direct_fabric_init - Set up the legacy switch fabric for direct ports.
 * @ppe_dev: PPE device.
 * @user_ports: Bit for each port that has a netdev.
 *
 * Each user port has a VSI of its own, whose members are the port and the
 * CPU port, so that the frames of a port are only forwarded to the CPU. The
 * default VSI 0 belongs to the CPU port and has every user port as a member.
 * Address learning is left off.
 *
 * Return: 0 on success, negative error code on failure.
 */
int ppe_direct_fabric_init(struct ppe_device *ppe_dev, u32 user_ports)
{
	u32 frame_size = ETH_HLEN + ETH_DATA_LEN + 2 * VLAN_HLEN;
	u32 port_mask = BIT(ppe_dev->num_ports) - 1;
	u32 cpu = BIT(PPE_CPU_PORT);
	unsigned long ports = user_ports;
	int port, vsi, ret;

	ret = regmap_write(ppe_dev->regmap, PPE_FDB_OP_ADDR, 0);
	if (ret)
		return ret;

	for (port = 0; port < ppe_dev->num_ports; port++) {
		ret = ppe_port_fabric_setup(ppe_dev, port, frame_size, port_mask);
		if (ret)
			return ret;
	}

	/* The CPU port keeps the default VSI, which is the one that the frames
	 * it sends are looked up in. Every user port must be a member.
	 */
	ppe_vsi_reserve(ppe_dev, 0);
	ret = ppe_vsi_set(ppe_dev, 0, user_ports | cpu, cpu, cpu, cpu);
	if (ret)
		return ret;

	for_each_set_bit(port, &ports, ppe_dev->num_ports) {
		vsi = ppe_vsi_alloc(ppe_dev);
		if (vsi < 0)
			return vsi;

		ret = ppe_vsi_set(ppe_dev, vsi, BIT(port) | cpu, cpu, cpu, cpu);
		if (ret)
			return ret;

		ret = ppe_user_port_setup(ppe_dev, port, vsi);
		if (ret)
			return ret;
	}

	ret = ppe_fdb_flush(ppe_dev);
	if (ret)
		return ret;

	return ppe_l2_egress_init(ppe_dev, user_ports);
}
