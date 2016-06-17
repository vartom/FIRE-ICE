/*
 * drivers/platform/tegra/pm_domains.c
 *
 * Copyright (c) 2012-2014, NVIDIA CORPORATION. All rights reserved.
 *
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/pm_domain.h>
#include <linux/tegra_pm_domains.h>
#include <linux/tegra-powergate.h>
#include <linux/platform_data/tegra_bpmp.h>
#include <linux/irqchip/tegra-agic.h>

#ifdef CONFIG_TEGRA_MC_DOMAINS
#define TEGRA_PD_DEV_CALLBACK(callback, dev)			\
({								\
	int (*__routine)(struct device *__d);			\
	int __ret = 0;						\
								\
	if (dev->type && dev->type->pm)				\
		__routine = dev->type->pm->callback;		\
	else if (dev->class && dev->class->pm)			\
		__routine = dev->class->pm->callback;		\
	else if (dev->bus && dev->bus->pm)			\
		__routine = dev->bus->pm->callback;		\
	else							\
		__routine = NULL;				\
								\
	if (!__routine && dev->driver && dev->driver->pm)	\
		__routine = dev->driver->pm->callback;		\
								\
	if (__routine)						\
		__ret = __routine(dev);				\
	__ret;							\
})

struct domain_client {
	const char *name;
	struct generic_pm_domain *domain;
};

#ifdef CONFIG_PM_SLEEP

static int tegra_pd_suspend_dev(struct device *dev)
{
	return TEGRA_PD_DEV_CALLBACK(suspend, dev);
}

static int tegra_pd_suspend_late(struct device *dev)
{
	return TEGRA_PD_DEV_CALLBACK(suspend_late, dev);
}

static int tegra_pd_resume_early(struct device *dev)
{
	return TEGRA_PD_DEV_CALLBACK(resume_early, dev);
}

static int tegra_pd_resume_dev(struct device *dev)
{
	return TEGRA_PD_DEV_CALLBACK(resume, dev);
}

static int tegra_pd_freeze_dev(struct device *dev)
{
	return TEGRA_PD_DEV_CALLBACK(freeze, dev);
}

static int tegra_pd_freeze_late(struct device *dev)
{
	return TEGRA_PD_DEV_CALLBACK(freeze_late, dev);
}

static int tegra_pd_thaw_early(struct device *dev)
{
	return TEGRA_PD_DEV_CALLBACK(thaw_early, dev);
}

static int tegra_pd_thaw_dev(struct device *dev)
{
	return TEGRA_PD_DEV_CALLBACK(thaw, dev);
}
#else /* !CONFIG_PM_SLEEP */

#define tegra_pd_suspend_dev	NULL
#define tegra_pd_suspend_late	NULL
#define tegra_pd_resume_early	NULL
#define tegra_pd_resume_dev	NULL
#define tegra_pd_freeze_dev	NULL
#define tegra_pd_freeze_late	NULL
#define tegra_pd_thaw_early	NULL
#define tegra_pd_thaw_dev	NULL

#endif /* !CONFIG_PM_SLEEP */

static bool tegra_pd_active_wakeup(struct device *dev)
{
	return device_may_wakeup(dev);
}

static int tegra_pd_save_dev(struct device *dev)
{
	return 0;
}

static int tegra_pd_restore_dev(struct device *dev)
{
	return 0;
}

static int tegra_pd_stop_dev(struct device *dev)
{
	return TEGRA_PD_DEV_CALLBACK(runtime_suspend, dev);
}

static int tegra_pd_start_dev(struct device *dev)
{
	return TEGRA_PD_DEV_CALLBACK(runtime_resume, dev);
}

struct gpd_dev_ops tegra_pd_ops = {
	.active_wakeup = tegra_pd_active_wakeup,
	.save_state = tegra_pd_save_dev,
	.restore_state = tegra_pd_restore_dev,
	.stop = tegra_pd_stop_dev,
	.start = tegra_pd_start_dev,
	.suspend = tegra_pd_suspend_dev,
	.suspend_late = tegra_pd_suspend_late,
	.resume_early = tegra_pd_resume_early,
	.resume = tegra_pd_resume_dev,
	.freeze = tegra_pd_freeze_dev,
	.freeze_late = tegra_pd_freeze_late,
	.thaw_early = tegra_pd_thaw_early,
	.thaw = tegra_pd_thaw_dev,
};

#ifdef CONFIG_ARCH_TEGRA_21x_SOC
static int tegra_mc_clk_power_off(struct generic_pm_domain *genpd)
{
	struct tegra_pm_domain *pd = to_tegra_pd(genpd);

	if (!pd)
		return -EINVAL;

	tegra_bpmp_scx_enable(true);

	return 0;
}

static int tegra_mc_clk_power_on(struct generic_pm_domain *genpd)
{
	struct tegra_pm_domain *pd = to_tegra_pd(genpd);

	if (!pd)
		return -EINVAL;

	tegra_bpmp_scx_enable(false);

	return 0;
}
#endif

#ifdef CONFIG_MMC_PM_DOMAIN
static void suspend_devices_in_domain(struct generic_pm_domain *genpd)
{
	struct pm_domain_data *pdd;
	struct device *dev;

	list_for_each_entry(pdd, &genpd->dev_list, list_node) {
		dev = pdd->dev;

		if (dev->pm_domain && dev->pm_domain->ops.suspend)
			dev->pm_domain->ops.suspend(dev);
	}
}

static void resume_devices_in_domain(struct generic_pm_domain *genpd)
{
	struct pm_domain_data *pdd;
	struct device *dev;
	list_for_each_entry(pdd, &genpd->dev_list, list_node) {
		dev = pdd->dev;

		if (dev->pm_domain && dev->pm_domain->ops.resume)
			dev->pm_domain->ops.resume(dev);
	}
}

static int tegra_core_power_on(struct generic_pm_domain *genpd)
{
	struct pm_domain_data *pdd;
	struct gpd_link *link;

	list_for_each_entry(link, &genpd->master_links, master_node)
		resume_devices_in_domain(link->slave);

	list_for_each_entry(pdd, &genpd->dev_list, list_node)
		TEGRA_PD_DEV_CALLBACK(resume, pdd->dev);
	return 0;
}

static int tegra_core_power_off(struct generic_pm_domain *genpd)
{
	struct pm_domain_data *pdd;
	struct gpd_link *link;

	list_for_each_entry(link, &genpd->master_links, master_node)
		suspend_devices_in_domain(link->slave);

	list_for_each_entry(pdd, &genpd->dev_list, list_node)
		TEGRA_PD_DEV_CALLBACK(suspend, pdd->dev);

	return 0;
}

static struct tegra_pm_domain tegra_sdhci3 = {
	.gpd.name = "tegra_sdhci.3",
	.gpd.power_off = tegra_core_power_off,
	.gpd.power_on = tegra_core_power_on,
	.gpd.power_off_delay = 5000,
};

static struct tegra_pm_domain tegra_sdhci2 = {
	.gpd.name = "tegra_sdhci.2",
	.gpd.power_off = tegra_core_power_off,
	.gpd.power_on = tegra_core_power_on,
	.gpd.power_off_delay = 5000,
};
#endif

static struct tegra_pm_domain tegra_mc_clk = {
	.gpd.name = "tegra_mc_clk",
#ifdef CONFIG_ARCH_TEGRA_21x_SOC
	.gpd.power_off = tegra_mc_clk_power_off,
	.gpd.power_on = tegra_mc_clk_power_on,
#endif
};

#ifndef CONFIG_ARCH_TEGRA_21x_SOC
static struct tegra_pm_domain tegra_nvavp = {
	.gpd.name = "tegra_nvavp",
};
#endif

#ifdef CONFIG_ARCH_TEGRA_21x_SOC

enum {
	APE_CLK = 0,
	APB2APE_CLK,
	ADSP_CLK,
	APE_MAX_CLK,
};

struct tegra_ape_clks {
	char *name;
	int idx;
};

static struct tegra_ape_clks ape_pd_clks[] = {
	{"ape", APE_CLK},
	{"apb2ape", APB2APE_CLK},
	{"adsp", ADSP_CLK},
};

static int tegra_ape_pd_enable_clks(struct tegra_pm_domain *ape_pd)
{
	int i;
	int ret = 0;

	for (i = 0; i < APE_MAX_CLK; i++) {
		int clk_idx = ape_pd_clks[i].idx;

		if (unlikely(!ape_pd->clk[clk_idx])) {
			struct clk *clk;

			clk = clk_get_sys(NULL, ape_pd_clks[i].name);
			if (IS_ERR_OR_NULL(clk)) {
				pr_err("%s:unable to find %s clock\n"
				       , __func__, ape_pd_clks[i].name);
				ret = PTR_ERR(clk);
				return ret;
			}
			ape_pd->clk[clk_idx] = clk;
		}

		ret = clk_prepare_enable(ape_pd->clk[clk_idx]);
		if (ret)
			return ret;
	}
	return ret;
}

static int tegra_ape_pd_disable_clks(struct tegra_pm_domain *ape_pd)
{
	int i;
	int ret = 0;

	for (i = 0; i < APE_MAX_CLK; i++) {
		int clk_idx = ape_pd_clks[i].idx;

		if (unlikely(!ape_pd->clk[clk_idx])) {
			struct clk *clk;

			clk = clk_get_sys(NULL, ape_pd_clks[i].name);
			if (IS_ERR_OR_NULL(clk)) {
				pr_err("%s:unable to find %s clock\n"
				       , __func__, ape_pd_clks[i].name);
				ret = PTR_ERR(clk);
				return ret;
			}
			ape_pd->clk[clk_idx] = clk;
		}

		/* The adsp clock is already disabled */
		if (clk_idx == ADSP_CLK)
			continue;

		clk_disable_unprepare(ape_pd->clk[clk_idx]);
	}
	return ret;
}

static int tegra_ape_power_on(struct generic_pm_domain *genpd)
{
	struct tegra_pm_domain *ape_pd;
	struct pm_domain_data *pdd;
	int ret = 0;

	ape_pd = to_tegra_pd(genpd);

	ret = tegra_ape_pd_enable_clks(ape_pd);
	if (ret)
		return ret;

	ret = tegra_unpowergate_partition(TEGRA_POWERGATE_APE);
	if (ret) {
		tegra_ape_pd_disable_clks(ape_pd);
		return ret;
	}

	/*
	 * The adsp clock shall be enabled only when ADSP cpu is
	 * on. ADSP cpu is switched on only for ADSP use-cases
	 * and the ADSP driver enables the adsp clock.
	 */
	clk_disable_unprepare(ape_pd->clk[ADSP_CLK]);

	tegra_agic_restore_registers();

	list_for_each_entry(pdd, &genpd->dev_list, list_node)
		TEGRA_PD_DEV_CALLBACK(resume, pdd->dev);

	return ret;
}

static int tegra_ape_power_off(struct generic_pm_domain *genpd)
{
	struct tegra_pm_domain *ape_pd;
	struct pm_domain_data *pdd;
	int ret = 0;

	list_for_each_entry(pdd, &genpd->dev_list, list_node)
		TEGRA_PD_DEV_CALLBACK(suspend, pdd->dev);

	tegra_agic_save_registers();

	ape_pd = to_tegra_pd(genpd);

	ret = tegra_ape_pd_disable_clks(ape_pd);
	if (ret)
		return ret;

	ret = tegra_powergate_partition(TEGRA_POWERGATE_APE);
	return ret;
}

static struct tegra_pm_domain tegra_ape = {
	.gpd.name = "tegra_ape",
	.gpd.power_off = tegra_ape_power_off,
	.gpd.power_on = tegra_ape_power_on,
};
#endif

static struct domain_client client_list[] = {
	{ .name = "tegradc", .domain = &tegra_mc_clk.gpd },
	{ .name = "tegra30-hda", .domain = &tegra_mc_clk.gpd },
	{ .name = "tegra-apbdma", .domain = &tegra_mc_clk.gpd },
	{ .name = "tegra-otg", .domain = &tegra_mc_clk.gpd },
	{ .name = "tegra-ehci", .domain = &tegra_mc_clk.gpd },
	{ .name = "tegra-xhci", .domain = &tegra_mc_clk.gpd },
	{ .name = "tegra-host1x", .domain = &tegra_mc_clk.gpd },
#ifndef CONFIG_ARCH_TEGRA_21x_SOC
	{ .name = "tegra_nvavp", .domain = &tegra_mc_clk.gpd },
	{ .name = "nvavp", .domain = &tegra_nvavp.gpd },
#endif
#ifdef CONFIG_MMC_PM_DOMAIN
	{ .name = "sdhci-tegra.3", .domain = &tegra_sdhci3.gpd },
	{ .name = "sdhci-tegra.2", .domain = &tegra_sdhci2.gpd },
	{ .name = "tegra_sdhci", .domain = &tegra_mc_clk.gpd },
#endif
	{ .name = "sdhci-tegra", .domain = &tegra_mc_clk.gpd },
	{ .name = "tegra11-se", .domain = &tegra_mc_clk.gpd },
	{ .name = "tegra12-se", .domain = &tegra_mc_clk.gpd },
	{ .name = "tegra-pcie", .domain = &tegra_mc_clk.gpd },
	{ .name = "gpu", .domain = &tegra_mc_clk.gpd },
#ifdef CONFIG_ARCH_TEGRA_21x_SOC
	{ .name = "tegra_ape", .domain = &tegra_mc_clk.gpd },
#endif
	{},
};

static int __init tegra_init_pm_domain(void)
{
	pm_genpd_init(&tegra_mc_clk.gpd, &simple_qos_governor, false);

#ifndef CONFIG_ARCH_TEGRA_21x_SOC
	pm_genpd_init(&tegra_nvavp.gpd, &simple_qos_governor, false);
	tegra_pd_add_sd(&tegra_nvavp.gpd);
#endif

#ifdef CONFIG_MMC_PM_DOMAIN
	/*
	 * Below Autosuspend delay can be increased/decreased based on
	 * power and perf data
	 */

	pm_genpd_init(&tegra_sdhci3.gpd, &simple_qos_governor, false);
	tegra_pd_add_sd(&tegra_sdhci3.gpd);
	pm_genpd_set_poweroff_delay(&tegra_sdhci3.gpd, 5000);

	pm_genpd_init(&tegra_sdhci2.gpd, &simple_qos_governor, false);
	tegra_pd_add_sd(&tegra_sdhci2.gpd);
	pm_genpd_set_poweroff_delay(&tegra_sdhci2.gpd, 5000);
#endif

#ifdef CONFIG_ARCH_TEGRA_21x_SOC
	pm_genpd_init(&tegra_ape.gpd, &simple_qos_governor, false);
	tegra_pd_add_sd(&tegra_ape.gpd);
	pm_genpd_set_poweroff_delay(&tegra_ape.gpd, 3000);
#endif
	return 0;
}
core_initcall(tegra_init_pm_domain);

static struct generic_pm_domain *tegra_pd_get_domain(const char *client)
{
	const char *s;
	struct domain_client *clients = client_list;

	while ((s = clients->name) != NULL) {
		if (!strncmp(s, client, strlen(s)))
			return clients->domain;

		clients++;
	}
	return NULL;
}

void tegra_pd_add_device(struct device *dev)
{
	struct generic_pm_domain *master = tegra_pd_get_domain(dev_name(dev));

	if (!master)
		return;

	device_set_wakeup_capable(dev, 1);
	pm_genpd_add_device(master, dev);
	pm_genpd_dev_need_save(dev, false);
	pm_genpd_add_callbacks(dev, &tegra_pd_ops, NULL);
}
EXPORT_SYMBOL(tegra_pd_add_device);

void tegra_pd_remove_device(struct device *dev)
{
	struct generic_pm_domain *genpd = dev_to_genpd(dev);

	if (!IS_ERR_OR_NULL(genpd))
		pm_genpd_remove_device(genpd, dev);
}
EXPORT_SYMBOL(tegra_pd_remove_device);

void tegra_pd_add_sd(struct generic_pm_domain *sd)
{
	struct generic_pm_domain *master = tegra_pd_get_domain(sd->name);

	if (!master)
		return;

	pm_genpd_add_subdomain(master, sd);
}
EXPORT_SYMBOL(tegra_pd_add_sd);

void tegra_pd_remove_sd(struct generic_pm_domain *sd)
{
	struct generic_pm_domain *master = tegra_pd_get_domain(sd->name);

	if (!master)
		return;

	pm_genpd_remove_subdomain(master, sd);
}
EXPORT_SYMBOL(tegra_pd_remove_sd);

#ifdef CONFIG_ARCH_TEGRA_21x_SOC
void tegra_ape_pd_add_device(struct device *dev)
{
	pm_genpd_add_device(&tegra_ape.gpd, dev);
	pm_genpd_dev_need_save(dev, true);
}
EXPORT_SYMBOL(tegra_ape_pd_add_device);

void tegra_ape_pd_remove_device(struct device *dev)
{
	pm_genpd_remove_device(&tegra_ape.gpd, dev);
}
EXPORT_SYMBOL(tegra_ape_pd_remove_device);
#endif

#else
struct tegra_pm_domain tegra_mc_clk;
EXPORT_SYMBOL(tegra_mc_clk);
struct tegra_pm_domain tegra_mc_chain_a;
EXPORT_SYMBOL(tegra_mc_chain_a);
struct tegra_pm_domain tegra_mc_chain_b;
EXPORT_SYMBOL(tegra_mc_chain_b);
#endif
