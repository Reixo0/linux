/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __SOC_QCOM_CRM_H__
#define __SOC_QCOM_CRM_H__

#include <linux/platform_device.h>

/**
 * enum crm_sw_drv_state:  Power states for the SW DRV request
 *
 * @CRM_ACTIVE_STATE:       Active or AMC mode requests. Resource state
 *                          is aggregated immediately.
 * @CRM_SLEEP_STATE:        State of the resource when the subsystem is
 *                          powered down. There is no client using the
 *                          resource actively.
 * @CRM_WAKE_STATE:         Resume resource state to the value previously
 *                          requested before the subsystem was powered down.
 */
enum crm_sw_drv_state {
	CRM_ACTIVE_STATE,
	CRM_SLEEP_STATE,
	CRM_WAKE_STATE,
};

/**
 * struct crm_cmd: The message to be sent to CRM
 *
 * @pwr_state:     The pwr_state for HW/SW DRV
 * @resource_idx:  The index of the VCD OR ND to apply data
 * @data:          The Clock Plan index for the VCDs voted by PERF_OL.
 *                 BW vote for the VCDs voted by BW.
 * @wait:          Set by the client want if want to wait for the vote completion IRQ.
 *                 Applicable for only SW DRV client.
 *                 Don't care for HW DRV client.
 */
struct crm_cmd {
	enum crm_sw_drv_state pwr_state;
	u32 resource_idx;
	u32 data;
	bool wait;
};

#if IS_ENABLED(CONFIG_QCOM_CRM)
int crm_write_perf_ol(const struct device *dev, u32 drv_id, const struct crm_cmd *cmd);
int crm_write_bw_vote(const struct device *dev, u32 drv_id, const struct crm_cmd *cmd);
int crm_write_bw_pt_vote(const struct device *dev, u32 drv_id, const struct crm_cmd *cmd);
struct device *crm_get(const struct device *dev);
#else

static inline int crm_write_perf_ol(const struct device *dev,
				    u32 drv_id,
				    const struct crm_cmd *cmd)
{ return -ENODEV; }

static inline int crm_write_bw_vote(const struct device *dev,
				    u32 drv_id,
				    const struct crm_cmd *cmd)
{ return -ENODEV; }

static inline int crm_write_bw_pt_vote(const struct device *dev,
				       u32 drv_id,
				       const struct crm_cmd *cmd)
{ return -ENODEV; }
static inline struct device *crm_get(const struct device *dev)
{ return NULL; }

#endif /* CONFIG_QCOM_CRM */

#endif /* __SOC_QCOM_CRM_H__ */
