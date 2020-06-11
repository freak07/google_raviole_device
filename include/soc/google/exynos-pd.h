/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 Samsung Electronics Co., Ltd.
 */

#ifndef __EXYNOS_PD_H
#define __EXYNOS_PD_H __FILE__

#include <linux/io.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/debugfs.h>

#include <linux/mfd/samsung/core.h>
#if IS_ENABLED(CONFIG_EXYNOS_BCM_DBG)
#include <soc/google/exynos-bcm_dbg.h>
#endif

#include <soc/google/exynos-cpupm.h>
#include <dt-bindings/power/exynos-power.h>

#define EXYNOS_PD_PREFIX	"EXYNOS-PD: "
#define EXYNOS_PD_DBG_PREFIX	"EXYNOS-PD-DBG: "

#ifndef pr_fmt
#define pr_fmt(fmt) fmt
#endif

#ifdef CONFIG_EXYNOS_PM_DOMAIN_DEBUG
#define DEBUG_PRINT_INFO(fmt, ...) \
	pr_info(EXYNOS_PD_DBG_PREFIX pr_fmt(fmt), ##__VA_ARGS__)
#else
#define DEBUG_PRINT_INFO(fmt, ...)
#endif

/* In Exynos, the number of MAX_POWER_DOMAIN is less than 15 */
#define MAX_PARENT_POWER_DOMAIN	15

struct exynos_pm_domain;

struct exynos_pm_domain {
	struct generic_pm_domain genpd;
	char *name;
	unsigned int cal_pdid;
	struct device_node *of_node;
	int (*pd_control)(unsigned int cal_id, int on);
	int (*check_status)(struct exynos_pm_domain *pd);
	bool (*power_down_ok)(void);
	unsigned int bts;
	int devfreq_index;
	struct mutex access_lock;
	int idle_ip_index;
#if IS_ENABLED(CONFIG_EXYNOS_BCM_DBG)
	struct exynos_bcm_pd_info *bcm;
#endif
	bool power_down_skipped;
	unsigned int need_smc;
	bool skip_idle_ip;
};

struct exynos_pd_dbg_info {
	struct device *dev;
#ifdef CONFIG_DEBUG_FS
	struct dentry *d;
	struct file_operations fops;
#endif
};

#if IS_ENABLED(CONFIG_EXYNOS_PD)
struct exynos_pm_domain *exynos_pd_lookup_name(const char *domain_name);
int exynos_pd_status(struct exynos_pm_domain *pd);
#else
static inline
struct exynos_pm_domain *exynos_pd_lookup_name(const char *domain_name)
{
	return NULL;
}
static inline int exynos_pd_status(struct exynos_pm_domain *pd)
{
	return 1;
}
#endif

#if IS_ENABLED(CONFIG_SND_SOC_SAMSUNG_VTS)
extern bool vts_is_on(void);
#endif
#if IS_ENABLED(CONFIG_SND_SOC_SAMSUNG_ABOX)
extern bool abox_is_on(void);
#endif
#ifdef CONFIG_USB_DWC3_EXYNOS
extern u32 otg_is_connect(void);
#else
static inline u32 otg_is_connect(void)
{
	return 0;
}
#endif

#endif /* __EXYNOS_PD_H */