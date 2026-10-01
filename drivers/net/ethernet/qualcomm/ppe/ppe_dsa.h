/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __PPE_DSA_H__
#define __PPE_DSA_H__

#include <linux/errno.h>

struct ppe_device;

#if IS_ENABLED(CONFIG_QCOM_PPE_DSA)
int ppe_dsa_switch_init(struct ppe_device *ppe_dev);
void ppe_dsa_switch_deinit(struct ppe_device *ppe_dev);
#else
static inline int ppe_dsa_switch_init(struct ppe_device *ppe_dev)
{
	return -EOPNOTSUPP;
}

static inline void ppe_dsa_switch_deinit(struct ppe_device *ppe_dev)
{
}
#endif

#endif /* __PPE_DSA_H__ */
