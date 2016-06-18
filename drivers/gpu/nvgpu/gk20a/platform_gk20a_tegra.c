/*
 * drivers/video/tegra/host/gk20a/platform_gk20a_tegra.c
 *
 * GK20A Tegra Platform Interface
 *
 * Copyright (c) 2014-2015, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/of_platform.h>
#include <linux/nvhost.h>
#include <linux/debugfs.h>
#include <linux/tegra-powergate.h>
#include <linux/platform_data/tegra_edp.h>
#include <uapi/linux/nvgpu.h>
#include <linux/dma-buf.h>
#include <linux/nvmap.h>
#include <linux/tegra_pm_domains.h>
#include <linux/platform/tegra/clock.h>

#include "gk20a.h"
#include "hal_gk20a.h"
#include "platform_gk20a.h"
#include "gk20a_scale.h"

#define TEGRA_GK20A_BW_PER_FREQ 32
#define TEGRA_GM20B_BW_PER_FREQ 64
#define TEGRA_DDR3_BW_PER_FREQ 16
#define TEGRA_DDR4_BW_PER_FREQ 16

extern struct device tegra_vpr_dev;
static struct gk20a_platform t132_gk20a_tegra_platform;

struct gk20a_emc_params {
	long bw_ratio;
};

#define MHZ_TO_HZ(x) ((x) * 1000000)
#define HZ_TO_MHZ(x) ((x) / 1000000)

static void gk20a_tegra_secure_page_destroy(struct platform_device *pdev,
				       struct secure_page_buffer *secure_buffer)
{
	dma_free_attrs(&tegra_vpr_dev, secure_buffer->size,
			(void *)(uintptr_t)secure_buffer->iova,
			secure_buffer->iova, &secure_buffer->attrs);
}

static int gk20a_tegra_secure_page_alloc(struct platform_device *pdev)
{
	struct gk20a_platform *platform = platform_get_drvdata(pdev);
	struct secure_page_buffer *secure_buffer = &platform->secure_buffer;
	DEFINE_DMA_ATTRS(attrs);
	dma_addr_t iova;
	size_t size = PAGE_SIZE;

	(void)dma_alloc_attrs(&tegra_vpr_dev, size, &iova,
				      DMA_MEMORY_NOMAP, &attrs);
	if (dma_mapping_error(&tegra_vpr_dev, iova))
		return -ENOMEM;

	secure_buffer->size = size;
	secure_buffer->iova = iova;
	secure_buffer->attrs = attrs;
	secure_buffer->destroy = gk20a_tegra_secure_page_destroy;

	return 0;
}

static void gk20a_tegra_secure_destroy(struct platform_device *pdev,
				       struct gr_ctx_buffer_desc *desc)
{
	gk20a_free_sgtable(&desc->sgt);
	dma_free_attrs(&tegra_vpr_dev, desc->size,
			(void *)(uintptr_t)desc->iova,
			desc->iova, &desc->attrs);
}

static int gk20a_tegra_secure_alloc(struct platform_device *pdev,
				    struct gr_ctx_buffer_desc *desc,
				    size_t size)
{
	struct gk20a_platform *platform = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;
	DEFINE_DMA_ATTRS(attrs);
	dma_addr_t iova;
	struct sg_table *sgt;
	struct page *page;
	int err = 0;

	if (!platform->secure_alloc_ready)
		return -EINVAL;

	(void)dma_alloc_attrs(&tegra_vpr_dev, size, &iova,
				      DMA_MEMORY_NOMAP, &attrs);
	if (dma_mapping_error(&tegra_vpr_dev, iova))
		return -ENOMEM;

	desc->iova = iova;
	desc->size = size;
	desc->attrs = attrs;
	desc->destroy = gk20a_tegra_secure_destroy;

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt) {
		gk20a_err(dev, "failed to allocate memory\n");
		goto fail;
	}
	err = sg_alloc_table(sgt, 1, GFP_KERNEL);
	if (err) {
		gk20a_err(dev, "failed to allocate sg_table\n");
		goto fail_sgt;
	}
	page = phys_to_page(iova);
	sg_set_page(sgt->sgl, page, size, 0);
	/* This bypasses SMMU for VPR during gmmu_map. */
	sg_dma_address(sgt->sgl) = 0;

	desc->sgt = sgt;

	return err;

fail_sgt:
	kfree(sgt);
fail:
	dma_free_attrs(&tegra_vpr_dev, desc->size,
			(void *)(uintptr_t)&desc->iova,
			desc->iova, &desc->attrs);
	return err;
}

/*
 * gk20a_tegra_get_emc_rate()
 *
 * This function returns the minimum emc clock based on gpu frequency
 */

static long gk20a_tegra_get_emc_rate(struct gk20a *g,
			      struct gk20a_emc_params *emc_params, long freq)
{
	long hz;

	freq = HZ_TO_MHZ(freq);

	hz = (freq * emc_params->bw_ratio);
	hz = (hz * min(g->pmu.load_avg, g->emc3d_ratio)) / 1000;

	hz = MHZ_TO_HZ(hz);

	return hz;
}

/*
 * gk20a_tegra_postscale(profile, freq)
 *
 * This function sets emc frequency based on current gpu frequency
 */

static void gk20a_tegra_postscale(struct platform_device *pdev,
				  unsigned long freq)
{
	struct gk20a_platform *platform = platform_get_drvdata(pdev);
	struct gk20a_scale_profile *profile = platform->g->scale_profile;
	struct gk20a_emc_params *emc_params = profile->private_data;
	struct gk20a *g = get_gk20a(pdev);

	long after = clk_get_rate(g->clk.tegra_clk);
	long emc_target = gk20a_tegra_get_emc_rate(g, emc_params, after);

	clk_set_rate(platform->clk[2], emc_target);
}

/*
 * gk20a_tegra_prescale(profile, freq)
 *
 * This function informs EDP about changed constraints.
 */

static void gk20a_tegra_prescale(struct platform_device *pdev)
{
	struct gk20a *g = get_gk20a(pdev);
	u32 avg = 0;

	gk20a_pmu_load_norm(g, &avg);
	tegra_edp_notify_gpu_load(avg, clk_get_rate(g->clk.tegra_clk));
}

/*
 * gk20a_tegra_calibrate_emc()
 *
 */

static void gk20a_tegra_calibrate_emc(struct platform_device *pdev,
			       struct gk20a_emc_params *emc_params)
{
	enum tegra_chipid cid = tegra_get_chipid();
	long gpu_bw, emc_bw;

	/* store gpu bw based on soc */
	switch (cid) {
	case TEGRA_CHIPID_TEGRA21:
		gpu_bw = TEGRA_GM20B_BW_PER_FREQ;
		break;
	case TEGRA_CHIPID_TEGRA12:
	case TEGRA_CHIPID_TEGRA13:
		gpu_bw = TEGRA_GK20A_BW_PER_FREQ;
		break;
	case TEGRA_CHIPID_UNKNOWN:
	default:
		gpu_bw = 0;
		break;
	}

	/* TODO detect DDR type.
	 * Okay for now since DDR3 and DDR4 have the same BW ratio */
	emc_bw = TEGRA_DDR3_BW_PER_FREQ;

	/* Calculate the bandwidth ratio of gpu_freq <-> emc_freq
	 *   NOTE the ratio must come out as an integer */
	emc_params->bw_ratio = (gpu_bw / emc_bw);
}

/*
 * gk20a_tegra_is_railgated()
 *
 * Check status of gk20a power rail
 */

static bool gk20a_tegra_is_railgated(struct platform_device *pdev)
{
	bool ret = false;

	if (!tegra_platform_is_linsim())
		ret = !tegra_powergate_is_powered(TEGRA_POWERGATE_GPU);

	return ret;
}

/*
 * gk20a_tegra_railgate()
 *
 * Gate (disable) gk20a power rail
 */

static int gk20a_tegra_railgate(struct platform_device *pdev)
{
	if (!tegra_platform_is_linsim() &&
	    tegra_powergate_is_powered(TEGRA_POWERGATE_GPU))
		tegra_powergate_partition(TEGRA_POWERGATE_GPU);
	return 0;
}

/*
 * gk20a_tegra_unrailgate()
 *
 * Ungate (enable) gk20a power rail
 */

static int gk20a_tegra_unrailgate(struct platform_device *pdev)
{
	int ret = 0;
	if (!tegra_platform_is_linsim())
		ret = tegra_unpowergate_partition(TEGRA_POWERGATE_GPU);
	return ret;
}

static struct {
	char *name;
	unsigned long default_rate;
} tegra_gk20a_clocks[] = {
	{"PLLG_ref", UINT_MAX},
	{"pwr", 204000000},
	{"emc", UINT_MAX} };

/*
 * gk20a_tegra_get_clocks()
 *
 * This function finds clocks in tegra platform and populates
 * the clock information to gk20a platform data.
 */

static int gk20a_tegra_get_clocks(struct platform_device *pdev)
{
	struct gk20a_platform *platform = platform_get_drvdata(pdev);
	char devname[16];
	int i;
	int ret = 0;

	snprintf(devname, sizeof(devname),
		 (pdev->id <= 0) ? "tegra_%s" : "tegra_%s.%d\n",
		 pdev->name, pdev->id);

	platform->num_clks = 0;
	for (i = 0; i < ARRAY_SIZE(tegra_gk20a_clocks); i++) {
		long rate = tegra_gk20a_clocks[i].default_rate;
		struct clk *c;

		c = clk_get_sys(devname, tegra_gk20a_clocks[i].name);
		if (IS_ERR(c)) {
			ret = PTR_ERR(c);
			goto err_get_clock;
		}
		rate = clk_round_rate(c, rate);
		clk_set_rate(c, rate);
		platform->clk[i] = c;
	}
	platform->num_clks = i;

	return 0;

err_get_clock:

	while (i--)
		clk_put(platform->clk[i]);
	return ret;
}

static int gk20a_tegra_reset_assert(struct platform_device *dev)
{
	struct gk20a_platform *platform = gk20a_get_platform(dev);

	if (!platform->clk_reset)
		platform->clk_reset = platform->clk[0];

	tegra_periph_reset_assert(platform->clk_reset);

	return 0;
}

static int gk20a_tegra_reset_deassert(struct platform_device *dev)
{
	struct gk20a_platform *platform = gk20a_get_platform(dev);

	if (!platform->clk_reset)
		return -EINVAL;

	tegra_periph_reset_deassert(platform->clk_reset);

	return 0;
}

static int gm20b_tegra_reset_assert(struct platform_device *dev)
{
	struct gk20a_platform *platform = gk20a_get_platform(dev);

	if (!platform->clk_reset) {
		platform->clk_reset = clk_get(&dev->dev, "gpu_gate");
		if (IS_ERR(platform->clk_reset)) {
			gk20a_err(&dev->dev, "fail to get gpu reset clk\n");
			return PTR_ERR(platform->clk_reset);
		}
	}

	tegra_periph_reset_assert(platform->clk_reset);

	return 0;
}

static void gk20a_tegra_scale_init(struct platform_device *pdev)
{
	struct gk20a_platform *platform = gk20a_get_platform(pdev);
	struct gk20a_scale_profile *profile = platform->g->scale_profile;
	struct gk20a_emc_params *emc_params;

	if (!profile)
		return;

	emc_params = kzalloc(sizeof(*emc_params), GFP_KERNEL);
	if (!emc_params)
		return;

	gk20a_tegra_calibrate_emc(pdev, emc_params);

	profile->private_data = emc_params;
}

static void gk20a_tegra_scale_exit(struct platform_device *pdev)
{
	struct gk20a_platform *platform = gk20a_get_platform(pdev);
	struct gk20a_scale_profile *profile = platform->g->scale_profile;

	if (profile)
		kfree(profile->private_data);
}

void gk20a_tegra_debug_dump(struct platform_device *pdev)
{
	struct gk20a_platform *platform = gk20a_get_platform(pdev);
	struct gk20a *g = platform->g;
	nvhost_debug_dump_device(g->host1x_dev);
}

int gk20a_tegra_busy(struct platform_device *dev)
{
	struct gk20a_platform *platform = gk20a_get_platform(dev);
	struct gk20a *g = platform->g;

	if (g->host1x_dev)
		return nvhost_module_busy_ext(g->host1x_dev);
	return 0;
}

void gk20a_tegra_idle(struct platform_device *dev)
{
	struct gk20a_platform *platform = gk20a_get_platform(dev);
	struct gk20a *g = platform->g;

	if (g->host1x_dev)
		nvhost_module_idle_ext(g->host1x_dev);
}

static int gk20a_tegra_probe(struct platform_device *dev)
{
	struct gk20a_platform *platform = gk20a_get_platform(dev);
	struct device_node *np = dev->dev.of_node;
	const __be32 *host1x_ptr;
	struct platform_device *host1x_pdev = NULL;

	host1x_ptr = of_get_property(np, "nvidia,host1x", NULL);
	if (host1x_ptr) {
		struct device_node *host1x_node =
			of_find_node_by_phandle(be32_to_cpup(host1x_ptr));

		host1x_pdev = of_find_device_by_node(host1x_node);
		if (!host1x_pdev) {
			dev_warn(&dev->dev, "host1x device not available");
			return -EPROBE_DEFER;
		}

	} else {
		host1x_pdev = to_platform_device(dev->dev.parent);
		dev_warn(&dev->dev, "host1x reference not found. assuming host1x to be parent");
	}

	platform->g->host1x_dev = host1x_pdev;

	if (tegra_get_chipid() == TEGRA_CHIPID_TEGRA13) {
		t132_gk20a_tegra_platform.g = platform->g;
		*platform = t132_gk20a_tegra_platform;
	}

	/* WAR for bug 1547668: Disable railgating and scaling irrespective of
	 * platform data if the rework has not been made. */

	if (tegra_get_chipid() == TEGRA_CHIPID_TEGRA21) {
		np = of_find_node_by_path("/gpu-dvfs-rework");
		if (!(np && of_device_is_available(np))) {
			platform->devfreq_governor = "";
			dev_warn(&dev->dev, "board does not support scaling");
		}
	}

	gk20a_tegra_get_clocks(dev);

	return 0;
}

static int gk20a_tegra_late_probe(struct platform_device *dev)
{
	struct gk20a_platform *platform = gk20a_get_platform(dev);

	/* Make gk20a power domain a subdomain of host1x */
	nvhost_register_client_domain(&platform->g->pd);

	/* Initialise tegra specific scaling quirks */
	gk20a_tegra_scale_init(dev);

	return 0;
}

static int gk20a_tegra_remove(struct platform_device *dev)
{
	struct gk20a_platform *platform = gk20a_get_platform(dev);

	/* remove gk20a power subdomain from host1x */
	nvhost_unregister_client_domain(&platform->g->pd);

	/* deinitialise tegra specific scaling quirks */
	gk20a_tegra_scale_exit(dev);

	return 0;
}

static int gk20a_tegra_suspend(struct device *dev)
{
	tegra_edp_notify_gpu_load(0, 0);
	return 0;
}

static struct gk20a_platform t132_gk20a_tegra_platform = {
	.has_syncpoints = true,

	/* power management configuration */
	.railgate_delay		= 500,
	.clockgate_delay	= 50,
	.can_railgate		= true,
	.enable_slcg            = true,
	.enable_blcg            = true,
	.enable_elcg            = true,
	.enable_elpg            = true,
	.enable_aelpg           = true,

	.force_reset_in_do_idle = false,

	.default_big_page_size	= SZ_128K,

	.probe = gk20a_tegra_probe,
	.late_probe = gk20a_tegra_late_probe,
	.remove = gk20a_tegra_remove,

	/* power management callbacks */
	.suspend = gk20a_tegra_suspend,
	.railgate = gk20a_tegra_railgate,
	.unrailgate = gk20a_tegra_unrailgate,
	.is_railgated = gk20a_tegra_is_railgated,

	.busy = gk20a_tegra_busy,
	.idle = gk20a_tegra_idle,

	.reset_assert = gk20a_tegra_reset_assert,
	.reset_deassert = gk20a_tegra_reset_deassert,

	/* frequency scaling configuration */
	.prescale = gk20a_tegra_prescale,
	.postscale = gk20a_tegra_postscale,
	.devfreq_governor = "nvhost_podgov",
	.qos_id = PM_QOS_GPU_FREQ_MIN,

	.secure_alloc = gk20a_tegra_secure_alloc,
	.secure_page_alloc = gk20a_tegra_secure_page_alloc,
	.dump_platform_dependencies = gk20a_tegra_debug_dump,
};

struct gk20a_platform gk20a_tegra_platform = {
	.has_syncpoints = true,

	/* power management configuration */
	.railgate_delay		= 500,
	.clockgate_delay	= 50,
	.can_railgate		= true,
	.enable_slcg            = true,
	.enable_blcg            = true,
	.enable_elcg            = true,
	.enable_elpg            = true,
	.enable_aelpg           = true,

	.force_reset_in_do_idle = false,

	.default_big_page_size	= SZ_128K,

	.probe = gk20a_tegra_probe,
	.late_probe = gk20a_tegra_late_probe,
	.remove = gk20a_tegra_remove,

	/* power management callbacks */
	.suspend = gk20a_tegra_suspend,
	.railgate = gk20a_tegra_railgate,
	.unrailgate = gk20a_tegra_unrailgate,
	.is_railgated = gk20a_tegra_is_railgated,

	.busy = gk20a_tegra_busy,
	.idle = gk20a_tegra_idle,

	.reset_assert = gk20a_tegra_reset_assert,
	.reset_deassert = gk20a_tegra_reset_deassert,

	/* frequency scaling configuration */
	.prescale = gk20a_tegra_prescale,
	.postscale = gk20a_tegra_postscale,
	.devfreq_governor = "nvhost_podgov",
	.qos_id = PM_QOS_GPU_FREQ_MIN,

	.secure_alloc = gk20a_tegra_secure_alloc,
	.secure_page_alloc = gk20a_tegra_secure_page_alloc,
	.dump_platform_dependencies = gk20a_tegra_debug_dump,
};

struct gk20a_platform gm20b_tegra_platform = {
	.has_syncpoints = true,

	/* power management configuration */
	.railgate_delay		= 500,
	.clockgate_delay	= 50,
	/* Disable all power features for gm20b */
	.can_railgate           = true,
	.enable_slcg            = true,
	.enable_blcg            = true,
	.enable_elcg            = true,
	.enable_elpg            = true,
	.enable_aelpg           = true,

	.force_reset_in_do_idle = false,

	.default_big_page_size	= SZ_128K,

	.probe = gk20a_tegra_probe,
	.late_probe = gk20a_tegra_late_probe,
	.remove = gk20a_tegra_remove,

	/* power management callbacks */
	.suspend = gk20a_tegra_suspend,
	.railgate = gk20a_tegra_railgate,
	.unrailgate = gk20a_tegra_unrailgate,
	.is_railgated = gk20a_tegra_is_railgated,

	.busy = gk20a_tegra_busy,
	.idle = gk20a_tegra_idle,

	.reset_assert = gm20b_tegra_reset_assert,
	.reset_deassert = gk20a_tegra_reset_deassert,

	/* frequency scaling configuration */
	.prescale = gk20a_tegra_prescale,
	.postscale = gk20a_tegra_postscale,
	.devfreq_governor = "nvhost_podgov",
	.qos_id = PM_QOS_GPU_FREQ_MIN,

	.secure_alloc = gk20a_tegra_secure_alloc,
	.secure_page_alloc = gk20a_tegra_secure_page_alloc,
	.dump_platform_dependencies = gk20a_tegra_debug_dump,

	.has_cde = true,
};
