# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for the device driver of PPE (Packet Process Engine) in IPQ SoC
#

obj-$(CONFIG_QCOM_PPE) += qcom-ppe.o
qcom-ppe-objs := ppe.o ppe_config.o ppe_debugfs.o ppe_mac.o edma.o
qcom-ppe-$(CONFIG_QCOM_PPE_DIRECT) += ppe_port.o
qcom-ppe-objs += edmav1/edma.o edmav1/cfg_rx.o edmav1/cfg_tx.o edmav1/rx.o edmav1/tx.o
qcom-ppe-objs += edmav2/edma.o edmav2/cfg_rx.o edmav2/cfg_tx.o edmav2/rx.o edmav2/tx.o
