/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __PPE_PORT_H__
#define __PPE_PORT_H__

#include <linux/errno.h>

struct ppe_device;

#if IS_ENABLED(CONFIG_QCOM_PPE_DIRECT)
int ppe_port_init(struct ppe_device *ppe_dev);
void ppe_port_deinit(struct ppe_device *ppe_dev);
#else
static inline int ppe_port_init(struct ppe_device *ppe_dev)
{
	return -EOPNOTSUPP;
}

static inline void ppe_port_deinit(struct ppe_device *ppe_dev)
{
}
#endif

#endif /* __PPE_PORT_H__ */
