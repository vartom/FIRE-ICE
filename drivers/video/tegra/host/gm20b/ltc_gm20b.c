/*
 * GM20B L2
 *
 * Copyright (c) 2014 NVIDIA CORPORATION.  All rights reserved.
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

#include <linux/types.h>

#include "hw_ltc_gm20b.h"
#include "hw_top_gm20b.h"
#include "hw_proj_gm20b.h"
#include "hw_pri_ringmaster_gm20b.h"

#include "gk20a/ltc_common.c"
#include "gk20a/gk20a.h"

static int gm20b_ltc_init_comptags(struct gk20a *g, struct gr_gk20a *gr)
{
	struct device *d = dev_from_gk20a(g);
	DEFINE_DMA_ATTRS(attrs);
	dma_addr_t iova;

	/* max memory size (MB) to cover */
	u32 max_size = gr->max_comptag_mem;
	/* one tag line covers 128KB */
	u32 max_comptag_lines = max_size << 3;

	u32 hw_max_comptag_lines =
		ltc_ltcs_ltss_cbc_ctrl3_clear_upper_bound_init_v();

	u32 cbc_param =
		gk20a_readl(g, ltc_ltcs_ltss_cbc_param_r());
	u32 comptags_per_cacheline =
		ltc_ltcs_ltss_cbc_param_comptags_per_cache_line_v(cbc_param);
	u32 cacheline_size =
		512 << ltc_ltcs_ltss_cbc_param_cache_line_size_v(cbc_param);
	u32 slices_per_ltc =
		ltc_ltcs_ltss_cbc_param_slices_per_ltc_v(cbc_param);

	u32 compbit_backing_size;

	nvhost_dbg_fn("");

	if (max_comptag_lines == 0) {
		gr->compbit_store.size = 0;
		return 0;
	}

	if (max_comptag_lines > hw_max_comptag_lines)
		max_comptag_lines = hw_max_comptag_lines;

	/* no hybird fb */
	compbit_backing_size =
		DIV_ROUND_UP(max_comptag_lines, comptags_per_cacheline) *
		cacheline_size * slices_per_ltc * gr->num_fbps;

	/* aligned to 2KB * num_fbps */
	compbit_backing_size +=
		gr->num_fbps << ltc_ltcs_ltss_cbc_base_alignment_shift_v();

	/* must be a multiple of 64KB */
	compbit_backing_size = roundup(compbit_backing_size, 64*1024);

	max_comptag_lines =
		(compbit_backing_size * comptags_per_cacheline) /
		cacheline_size * slices_per_ltc * gr->num_fbps;

	if (max_comptag_lines > hw_max_comptag_lines)
		max_comptag_lines = hw_max_comptag_lines;

	nvhost_dbg_info("compbit backing store size : %d",
		compbit_backing_size);
	nvhost_dbg_info("max comptag lines : %d",
		max_comptag_lines);

	dma_set_attr(DMA_ATTR_NO_KERNEL_MAPPING, &attrs);
	gr->compbit_store.size = compbit_backing_size;
	gr->compbit_store.pages = dma_alloc_attrs(d, gr->compbit_store.size,
						  &iova, GFP_KERNEL, &attrs);
	if (!gr->compbit_store.pages) {
		nvhost_err(dev_from_gk20a(g), "failed to allocate"
			   "backing store for compbit : size %d",
			   compbit_backing_size);
		return -ENOMEM;
	}
	gr->compbit_store.base_iova = iova;

	nvhost_allocator_init(&gr->comp_tags, "comptag",
			      1, /* start */
			      max_comptag_lines - 1, /* length*/
			      1); /* align */

	return 0;
}

static int gm20b_ltc_clear_comptags(struct gk20a *g, u32 min, u32 max)
{
	struct gr_gk20a *gr = &g->gr;
	u32 fbp, slice, ctrl1, val;
	unsigned long end_jiffies = jiffies +
		msecs_to_jiffies(gk20a_get_gr_idle_timeout(g));
	u32 delay = GR_IDLE_CHECK_DEFAULT;
	u32 slices_per_ltc = ltc_ltcs_ltss_cbc_param_slices_per_ltc_v(
				gk20a_readl(g, ltc_ltcs_ltss_cbc_param_r()));

	nvhost_dbg_fn("");

	if (gr->compbit_store.size == 0)
		return 0;

	gk20a_writel(g, ltc_ltcs_ltss_cbc_ctrl2_r(),
		     ltc_ltcs_ltss_cbc_ctrl2_clear_lower_bound_f(min));
	gk20a_writel(g, ltc_ltcs_ltss_cbc_ctrl3_r(),
		     ltc_ltcs_ltss_cbc_ctrl3_clear_upper_bound_f(max));
	gk20a_writel(g, ltc_ltcs_ltss_cbc_ctrl1_r(),
		     gk20a_readl(g, ltc_ltcs_ltss_cbc_ctrl1_r()) |
		     ltc_ltcs_ltss_cbc_ctrl1_clear_active_f());

	for (fbp = 0; fbp < gr->num_fbps; fbp++) {
		for (slice = 0; slice < slices_per_ltc; slice++) {

			delay = GR_IDLE_CHECK_DEFAULT;

			ctrl1 = ltc_ltc0_lts0_cbc_ctrl1_r() +
				fbp * proj_ltc_stride_v() +
				slice * proj_lts_stride_v();

			do {
				val = gk20a_readl(g, ctrl1);
				if (ltc_ltcs_ltss_cbc_ctrl1_clear_v(val) !=
				    ltc_ltcs_ltss_cbc_ctrl1_clear_active_v())
					break;

				usleep_range(delay, delay * 2);
				delay = min_t(u32, delay << 1,
					GR_IDLE_CHECK_MAX);

			} while (time_before(jiffies, end_jiffies) |
					!tegra_platform_is_silicon());

			if (!time_before(jiffies, end_jiffies)) {
				nvhost_err(dev_from_gk20a(g),
					   "comp tag clear timeout\n");
				return -EBUSY;
			}
		}
	}

	return 0;
}

static void gm20b_ltc_init_fs_state(struct gk20a *g)
{
	nvhost_dbg_info("initialize gm20b l2");

	g->max_ltc_count = gk20a_readl(g, top_num_ltcs_r());
	g->ltc_count = gk20a_readl(g, pri_ringmaster_enum_ltc_r());
	nvhost_dbg_info("%d ltcs out of %d", g->ltc_count, g->max_ltc_count);

	gk20a_writel(g, ltc_ltcs_ltss_cbc_num_active_ltcs_r(),
	g->ltc_count);
	gk20a_writel(g, ltc_ltcs_misc_ltc_num_active_ltcs_r(),
	g->ltc_count);
}

void gm20b_init_ltc(struct gpu_ops *gops)
{
	/* Gk20a reused ops. */
	gops->ltc.determine_L2_size_bytes = gk20a_determine_L2_size_bytes;
	gops->ltc.set_max_ways_evict_last = gk20a_ltc_set_max_ways_evict_last;
	gops->ltc.set_zbc_color_entry = gk20a_ltc_set_zbc_color_entry;
	gops->ltc.set_zbc_depth_entry = gk20a_ltc_set_zbc_depth_entry;
	gops->ltc.clear_zbc_color_entry = gk20a_ltc_clear_zbc_color_entry;
	gops->ltc.clear_zbc_depth_entry = gk20a_ltc_clear_zbc_depth_entry;
	gops->ltc.init_zbc = gk20a_ltc_init_zbc;
	gops->ltc.init_cbc = gk20a_ltc_init_cbc;

	/* GM20b specific ops. */
	gops->ltc.init_fs_state = gm20b_ltc_init_fs_state;
	gops->ltc.init_comptags = gm20b_ltc_init_comptags;
	gops->ltc.clear_comptags = gm20b_ltc_clear_comptags;
	gops->ltc.elpg_flush = gk20a_mm_g_elpg_flush_locked;
}
