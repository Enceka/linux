// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2019 Spreadtrum Communications Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifdef pr_fmt
#undef pr_fmt
#endif
#define pr_fmt(fmt) "sipa_dele: " fmt

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/sipa.h>
#include <linux/sipc.h>

#include "sipa_dele_priv.h"

static struct cp_delegator *s_cp_delegator;

static void cp_dele_on_commad(void *priv, u16 flag, u32 data)
{
	struct sipa_delegator *delegator = priv;
	int ret = 0;

	/* Serialize CP commands against reset work; PM resume may sleep. */
	mutex_lock(&delegator->power_lock);
	switch (flag) {
	case SMSG_FLG_DELE_ENABLE:
		/* Duplicate ENABLE needs an ACK, not another power reference. */
		if (delegator->pd_get_flag)
			goto enabled;
		if (delegator->cfg->sipa_sys_eb) {
			ret = pm_runtime_resume_and_get(delegator->pdev);
			if (ret < 0)
				goto enable_failed;
		} else {
			pm_stay_awake(delegator->pdev);
		}
		ret = sipa_set_enabled(true);
		if (ret < 0) {
			/* set_enabled() increments its reference even on HAL failure. */
			sipa_set_enabled(false);
			if (delegator->cfg->sipa_sys_eb)
				pm_runtime_put(delegator->pdev);
			else
				pm_relax(delegator->pdev);
			goto enable_failed;
		}
		delegator->pd_get_flag = true;
enabled:
		delegator->pd_eb_flag = true;
		sipa_dele_start_done_work(delegator, SMSG_FLG_DELE_ENABLE,
					  SMSG_VAL_DELE_REQ_SUCCESS);
		break;
enable_failed:
		delegator->pd_eb_flag = false;
		dev_warn_ratelimited(delegator->pdev, "SIPA enable failed: %d\n", ret);
		sipa_dele_start_done_work(delegator, SMSG_FLG_DELE_ENABLE,
					  SMSG_VAL_DELE_REQ_FAIL);
		break;
	case SMSG_FLG_DELE_DISABLE:
		delegator->pd_eb_flag = false;
		if (delegator->pd_get_flag) {
			sipa_set_enabled(false);
			if (delegator->cfg->sipa_sys_eb)
				pm_runtime_put(delegator->pdev);
			else
				pm_relax(delegator->pdev);
			delegator->pd_get_flag = false;
		}
		break;
	default:
		break;
	}
	sipa_dele_on_commad(priv, flag, data);
	mutex_unlock(&delegator->power_lock);
}

static void cp_dele_on_event(void *priv, u16 flag, u32 data)
{
	struct sipa_delegator *delegator = priv;

	pr_info("prod_id:%d, on_evt, flag = %d\n", delegator->prod_id, flag);

	switch (flag) {
	case SMSG_FLG_DELE_ENTER_FLOWCTRL:
		sipa_irq_affinity_change(true);
		break;
	case SMSG_FLG_DELE_EXIT_FLOWCTRL:
		break;
	default:
		break;
	}
}

static int cp_dele_local_req_r_prod(void *user_data)
{
	/* do enable ipa  operation */

	return sipa_dele_local_req_r_prod(user_data);
}

static void cp_dele_restart_handler(struct work_struct *work)
{
	struct delayed_work *restart_delayed_work = to_delayed_work(work);
	struct sipa_delegator *delegator = container_of(restart_delayed_work,
					struct sipa_delegator, restart_work);

	mutex_lock(&delegator->power_lock);
	if (delegator->pd_get_flag) {
		sipa_set_enabled(false);
		if (delegator->cfg->sipa_sys_eb)
			pm_runtime_put(delegator->pdev);
		else
			pm_relax(delegator->pdev);
		delegator->pd_get_flag = false;
	}
	delegator->pd_eb_flag = false;
	if (delegator->cons_ref_cnt) {
		delegator->cons_ref_cnt--;
		sipa_rm_release_resource(delegator->cons_user);
	}
	mutex_unlock(&delegator->power_lock);
}

static ssize_t sipa_dele_reset_show(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	char *a = "modem reset\n";

	return sprintf(buf, "\n%s\n", a);
}

static ssize_t sipa_dele_reset_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf,
				     size_t count)
{
	struct sipa_delegator *delegator = &s_cp_delegator->delegator;

	if (strstr(buf, "Modem Assert") &&
	    !strstr(buf, "P-ARM Modem Assert")) {
		if (!delegator->cfg->sipa_sys_eb) {
			dev_info(delegator->pdev, "Modem assert\n");
			cancel_delayed_work(&delegator->restart_work);
		}
	} else if (strstr(buf, "Modem Reset") ||
		   strstr(buf, "Modem Blocked")) {
		dev_info(delegator->pdev, "reset sipa_dele for modem\n");
		cancel_delayed_work(&delegator->restart_work);
		queue_delayed_work(delegator->restart_wq,
				   &delegator->restart_work,
				   0);
	}

	dev_info(delegator->pdev, "modem cmd %s\n", buf);

	return count;
}

static DEVICE_ATTR_RW(sipa_dele_reset);

static struct attribute *sipa_dele_attrs[] = {
	&dev_attr_sipa_dele_reset.attr,
	NULL,
};
ATTRIBUTE_GROUPS(sipa_dele);

static int sipa_dele_init_sysfs(struct cp_delegator *cp_delegator)
{
	int ret;
	struct sipa_delegator *delegator = &cp_delegator->delegator;

	ret = sysfs_create_groups(&delegator->pdev->kobj,
				  sipa_dele_groups);
	if (ret) {
		dev_err(delegator->pdev, "sipa_dele fail to create sysfs\n");
		sysfs_remove_groups(&delegator->pdev->kobj,
				    sipa_dele_groups);
		return ret;
	}

	return 0;
}

int cp_delegator_init(struct sipa_delegator_create_params *params)
{
	int ret;

	s_cp_delegator = devm_kzalloc(params->pdev,
				      sizeof(*s_cp_delegator),
				      GFP_KERNEL);
	if (!s_cp_delegator)
		return -ENOMEM;
	ret = sipa_delegator_init(&s_cp_delegator->delegator,
				  params);
	if (ret)
		return ret;

	s_cp_delegator->delegator.on_cmd = cp_dele_on_commad;
	s_cp_delegator->delegator.on_evt = cp_dele_on_event;
	s_cp_delegator->delegator.local_request_prod = cp_dele_local_req_r_prod;
	s_cp_delegator->delegator.pd_eb_flag = false;
	s_cp_delegator->delegator.pd_get_flag = false;
	s_cp_delegator->delegator.smsg_cnt = 0;

	if (s_cp_delegator->delegator.cfg->sipa_sys_eb)
		pm_runtime_enable(s_cp_delegator->delegator.pdev);
	s_cp_delegator->delegator.restart_wq = create_workqueue("sipa_dele_restart_wq");
	if (!s_cp_delegator->delegator.restart_wq) {
		dev_err(s_cp_delegator->delegator.pdev,
			"sipa_dele_restart_wq create failed\n");
		sipa_delegator_exit(&s_cp_delegator->delegator);
		if (s_cp_delegator->delegator.cfg->sipa_sys_eb)
			pm_runtime_disable(s_cp_delegator->delegator.pdev);
		return -ENOMEM;
	}
	INIT_DELAYED_WORK(&s_cp_delegator->delegator.restart_work,
			  cp_dele_restart_handler);
	mutex_init(&s_cp_delegator->delegator.power_lock);
	ret = sipa_delegator_start(&s_cp_delegator->delegator);
	if (ret) {
		destroy_workqueue(s_cp_delegator->delegator.restart_wq);
		sipa_delegator_exit(&s_cp_delegator->delegator);
		if (s_cp_delegator->delegator.cfg->sipa_sys_eb)
			pm_runtime_disable(s_cp_delegator->delegator.pdev);
		return ret;
	}

	ret = sipa_rm_add_dependency(SIPA_RM_RES_CONS_WIFI_UL,
				     s_cp_delegator->delegator.prod_id);
	if (ret && ret != -EINPROGRESS)
		dev_warn(params->pdev, "CONS_WIFI_UL dependency: %d\n", ret);

	ret = sipa_rm_add_dependency(SIPA_RM_RES_CONS_WIFI_DL,
				     s_cp_delegator->delegator.prod_id);
	if (ret && ret != -EINPROGRESS)
		dev_warn(params->pdev, "CONS_WIFI_DL dependency: %d\n", ret);

	sipa_dele_init_sysfs(s_cp_delegator);
	/* All callbacks, power state and resources are ready before CP commands. */
	wake_up_process(s_cp_delegator->delegator.thread);

	return 0;
}
