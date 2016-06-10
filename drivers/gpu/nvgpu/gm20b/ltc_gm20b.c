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
#include <linux/jiffies.h>

#include "hw_mc_gm20b.h"
#include "hw_ltc_gm20b.h"
#include "hw_top_gm20b.h"
#include "hw_proj_gm20b.h"
#include "hw_pri_ringmaster_gm20b.h"

#include "gk20a/ltc_common.c"
#include "gk20a/gk20a.h"
#include "gk20a/gk20a_allocator.h"

static int gm20b_ltc_init_comptags(struct gk20a *g, struct gr_gk20a *gr)
{
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

	int err;

	gk20a_dbg_fn("");

	if (max_comptag_lines == 0) {
		gr->compbit_store.size = 0;
		return 0;
	}

	if (max_comptag_lines > hw_max_comptag_lines)
		max_comptag_lines = hw_max_comptag_lines;

	compbit_backing_size =
		DIV_ROUND_UP(max_comptag_lines, comptags_per_cacheline) *
		cacheline_size * slices_per_ltc * g->ltc_count;

	/* aligned to 2KB * ltc_count */
	compbit_backing_size +=
		g->ltc_count << ltc_ltcs_ltss_cbc_base_alignment_shift_v();

	/* must be a multiple of 64KB */
	compbit_backing_size = roundup(compbit_backing_size, 64*1024);

	max_comptag_lines =
		(compbit_backing_size * comptags_per_cacheline) /
		(cacheline_size * slices_per_ltc * g->ltc_count);

	if (max_comptag_lines > hw_max_comptag_lines)
		max_comptag_lines = hw_max_comptag_lines;

	nvhost_dbg_info("compbit backing store size : %d",
		compbit_backing_size);
	nvhost_dbg_info("max comptag lines : %d",
		max_comptag_lines);

	if (tegra_platform_is_linsim())
		err = gk20a_ltc_alloc_phys_cbc(g, compbit_backing_size);
	else
		err = gk20a_ltc_alloc_virt_cbc(g, compbit_backing_size);

	if (err)
		return err;

	gk20a_allocator_init(&gr->comp_tags, "comptag",
			      1, /* start */
			      max_comptag_lines - 1, /* length*/
			      1); /* align */

	gr->comptags_per_cacheline = comptags_per_cacheline;
	gr->slices_per_ltc = slices_per_ltc;
	gr->cacheline_size = cacheline_size;

	return 0;
}

static int gm20b_ltc_cbc_ctrl(struct gk20a *g, enum gk20a_cbc_op op,
			      u32 min, u32 max)
{
	int err = 0;
	struct gr_gk20a *gr = &g->gr;
	u32 ltc, slice, ctrl1, val, hw_op = 0;
	unsigned long end_jiffies = jiffies +
		msecs_to_jiffies(gk20a_get_gr_idle_timeout(g));
	u32 delay = GR_IDLE_CHECK_DEFAULT;
	u32 slices_per_ltc = ltc_ltcs_ltss_cbc_param_slices_per_ltc_v(
				gk20a_readl(g, ltc_ltcs_ltss_cbc_param_r()));

	nvhost_dbg_fn("");

	if (gr->compbit_store.size == 0)
		return 0;

	mutex_lock(&g->mm.l2_op_lock);

	if (op == gk20a_cbc_op_clear) {
		gk20a_writel(g, ltc_ltcs_ltss_cbc_ctrl2_r(),
			ltc_ltcs_ltss_cbc_ctrl2_clear_lower_bound_f(min));
		gk20a_writel(g, ltc_ltcs_ltss_cbc_ctrl3_r(),
			ltc_ltcs_ltss_cbc_ctrl3_clear_upper_bound_f(max));
		hw_op = ltc_ltcs_ltss_cbc_ctrl1_clear_active_f();
	} else if (op == gk20a_cbc_op_clean) {
		hw_op = ltc_ltcs_ltss_cbc_ctrl1_clean_active_f();
	} else if (op == gk20a_cbc_op_invalidate) {
		hw_op = ltc_ltcs_ltss_cbc_ctrl1_invalidate_active_f();
	} else {
		BUG_ON(1);
	}
	gk20a_writel(g, ltc_ltcs_ltss_cbc_ctrl1_r(),
		     gk20a_readl(g, ltc_ltcs_ltss_cbc_ctrl1_r()) | hw_op);

	for (ltc = 0; ltc < g->ltc_count; ltc++) {
		for (slice = 0; slice < slices_per_ltc; slice++) {

			delay = GR_IDLE_CHECK_DEFAULT;

			ctrl1 = ltc_ltc0_lts0_cbc_ctrl1_r() +
				ltc * proj_ltc_stride_v() +
				slice * proj_lts_stride_v();

			do {
				val = gk20a_readl(g, ctrl1);
				if (!(val & hw_op))
					break;

				usleep_range(delay, delay * 2);
				delay = min_t(u32, delay << 1,
					GR_IDLE_CHECK_MAX);

			} while (time_before(jiffies, end_jiffies) |
					!tegra_platform_is_silicon());

			if (!time_before(jiffies, end_jiffies)) {
				nvhost_err(dev_from_gk20a(g),
					   "comp tag clear timeout\n");
				err = -EBUSY;
				goto out;
			}
		}
	}
out:
	mutex_unlock(&g->mm.l2_op_lock);
	return 0;
}

static void gm20b_ltc_init_fs_state(struct gk20a *g)
{
	u32 reg;

	gk20a_dbg_info("initialize gm20b l2");

	g->max_ltc_count = gk20a_readl(g, top_num_ltcs_r());
	g->ltc_count = gk20a_readl(g, pri_ringmaster_enum_ltc_r());
	nvhost_dbg_info("%d ltcs out of %d", g->ltc_count, g->max_ltc_count);

	gk20a_writel(g, ltc_ltcs_ltss_cbc_num_active_ltcs_r(),
	g->ltc_count);
	gk20a_writel(g, ltc_ltcs_misc_ltc_num_active_ltcs_r(),
	g->ltc_count);
	gk20a_writel(g, ltc_ltcs_ltss_dstg_cfg0_r(),
		     gk20a_readl(g, ltc_ltc0_lts0_dstg_cfg0_r()) |
		     ltc_ltcs_ltss_dstg_cfg0_vdc_4to2_disable_m());

	/* Disable LTC interrupts */
	reg = gk20a_readl(g, ltc_ltcs_ltss_intr_r());
	reg &= ~ltc_ltcs_ltss_intr_en_evicted_cb_m();
	reg &= ~ltc_ltcs_ltss_intr_en_illegal_compstat_access_m();
	gk20a_writel(g, ltc_ltcs_ltss_intr_r(), reg);
}

void gm20b_ltc_isr(struct gk20a *g)
{
	u32 mc_intr, ltc_intr;
	int ltc, slice;

	mc_intr = gk20a_readl(g, mc_intr_ltc_r());
	gk20a_err(dev_from_gk20a(g), "mc_ltc_intr: %08x",
		  mc_intr);
	for (ltc = 0; ltc < g->ltc_count; ltc++) {
		if ((mc_intr & 1 << ltc) == 0)
			continue;
		for (slice = 0; slice < g->gr.slices_per_ltc; slice++) {
			ltc_intr = gk20a_readl(g, ltc_ltc0_lts0_intr_r() +
					   proj_ltc_stride_v() * ltc +
					   proj_lts_stride_v() * slice);
			gk20a_err(dev_from_gk20a(g), "ltc%d, slice %d: %08x",
				  ltc, slice, ltc_intr);
			gk20a_writel(g, ltc_ltc0_lts0_intr_r() +
					   proj_ltc_stride_v() * ltc +
					   proj_lts_stride_v() * slice,
				     ltc_intr);
		}
	}
}

static void gm20b_ltc_g_elpg_flush_locked(struct gk20a *g)
{
	u32 data;
	bool done[g->ltc_count];
	s32 retry = 100;
	int i;
	int num_done = 0;
	u32 ltc_d = ltc_ltc1_ltss_g_elpg_r() - ltc_ltc0_ltss_g_elpg_r();

	gk20a_dbg_fn("");

	for (i = 0; i < g->ltc_count; i++)
		done[i] = 0;

	gk20a_writel(g, ltc_ltcs_ltss_g_elpg_r(),
		     ltc_ltcs_ltss_g_elpg_flush_pending_f());
	do {
		for (i = 0; i < g->ltc_count; i++) {
			if (done[i])
				continue;

			data = gk20a_readl(g,
					ltc_ltc0_ltss_g_elpg_r() + ltc_d * i);

			if (ltc_ltc0_ltss_g_elpg_flush_v(data)) {
				gk20a_dbg_info("g_elpg_flush 0x%x", data);
			} else {
				done[i] = 1;
				num_done++;
			}
		}

		if (num_done < g->ltc_count) {
			retry--;
			usleep_range(20, 40);
		} else
			break;
	} while (retry >= 0 || !tegra_platform_is_silicon());

	if (retry < 0)
		gk20a_warn(dev_from_gk20a(g),
			    "g_elpg_flush too many retries");
}

u32 gm20b_ltc_cbc_fix_config(struct gk20a *g, int base)
{
	u32 val = gk20a_readl(g, ltc_ltcs_ltss_cbc_num_active_ltcs_r());
	if (val == 2) {
		return base * 2;
	} else if (val != 1) {
		gk20a_err(dev_from_gk20a(g),
			"Invalid number of active ltcs: %08x\n", val);
	}

	return base;
}

/*
 * Performs a full flush of the L2 cache.
 */
void gm20b_flush_ltc(struct gk20a *g)
{
	u32 op_pending;
	unsigned long now, timeout;

#define __timeout_init()				\
	do {						\
		now = jiffies;	timeout = now + HZ;	\
	} while (0)
#define __timeout_check()						\
	do {								\
		if (tegra_platform_is_silicon() && time_after(now, timeout)) { \
			gk20a_err(dev_from_gk20a(g), "L2 flush timeout!"); \
			break;						\
		}							\
	} while (0)

	/* Clean... */
	gk20a_writel(g, ltc_ltcs_ltss_tstg_cmgmt1_r(),
		ltc_ltcs_ltss_tstg_cmgmt1_clean_pending_f() |
		ltc_ltcs_ltss_tstg_cmgmt1_max_cycles_between_cleans_3_f() |
		ltc_ltcs_ltss_tstg_cmgmt1_clean_wait_for_fb_to_pull_true_f() |
		ltc_ltcs_ltss_tstg_cmgmt1_clean_evict_last_class_true_f() |
		ltc_ltcs_ltss_tstg_cmgmt1_clean_evict_normal_class_true_f() |
		ltc_ltcs_ltss_tstg_cmgmt1_clean_evict_first_class_true_f());

	/* Wait on each LTC individually. */
	__timeout_init();
	do {
		op_pending = gk20a_readl(g, ltc_ltc0_ltss_tstg_cmgmt1_r());
		__timeout_check();
	} while (op_pending &
		 ltc_ltc0_ltss_tstg_cmgmt1_clean_pending_f());

	__timeout_init();
	do {
		op_pending = gk20a_readl(g, ltc_ltc1_ltss_tstg_cmgmt1_r());
		__timeout_check();
	} while (op_pending &
		 ltc_ltc1_ltss_tstg_cmgmt1_clean_pending_f());

	/* And invalidate. */
	gk20a_writel(g, ltc_ltcs_ltss_tstg_cmgmt0_r(),
	     ltc_ltcs_ltss_tstg_cmgmt0_invalidate_pending_f() |
	     ltc_ltcs_ltss_tstg_cmgmt0_max_cycles_between_invalidates_3_f() |
	     ltc_ltcs_ltss_tstg_cmgmt0_invalidate_evict_last_class_true_f() |
	     ltc_ltcs_ltss_tstg_cmgmt0_invalidate_evict_normal_class_true_f() |
	     ltc_ltcs_ltss_tstg_cmgmt0_invalidate_evict_first_class_true_f());

	/* Wait on each LTC individually. */
	__timeout_init();
	do {
		op_pending = gk20a_readl(g, ltc_ltc0_ltss_tstg_cmgmt0_r());
		__timeout_check();
	} while (op_pending &
		 ltc_ltc0_ltss_tstg_cmgmt0_invalidate_pending_f());

	__timeout_init();
	do {
		op_pending = gk20a_readl(g, ltc_ltc1_ltss_tstg_cmgmt0_r());
		__timeout_check();
	} while (op_pending &
		 ltc_ltc1_ltss_tstg_cmgmt0_invalidate_pending_f());
}

static int gm20b_determine_L2_size_bytes(struct gk20a *g)
{
	u32 lts_per_ltc;
	u32 ways;
	u32 sets;
	u32 bytes_per_line;
	u32 active_ltcs;
	u32 cache_size;

	u32 tmp;
	u32 active_sets_value;

	tmp = gk20a_readl(g, ltc_ltc0_lts0_tstg_cfg1_r());
	ways = hweight32(ltc_ltc0_lts0_tstg_cfg1_active_ways_v(tmp));

	active_sets_value = ltc_ltc0_lts0_tstg_cfg1_active_sets_v(tmp);
	if (active_sets_value == ltc_ltc0_lts0_tstg_cfg1_active_sets_all_v()) {
		sets = 64;
	} else if (active_sets_value ==
		 ltc_ltc0_lts0_tstg_cfg1_active_sets_half_v()) {
		sets = 32;
	} else if (active_sets_value ==
		 ltc_ltc0_lts0_tstg_cfg1_active_sets_quarter_v()) {
		sets = 16;
	} else {
		dev_err(dev_from_gk20a(g),
			"Unknown constant %u for active sets",
		       (unsigned)active_sets_value);
		sets = 0;
	}

	active_ltcs = g->gr.num_fbps;

	/* chip-specific values */
	lts_per_ltc = 2;
	bytes_per_line = 128;
	cache_size = active_ltcs * lts_per_ltc * ways * sets * bytes_per_line;

	return cache_size;
}

static struct gpu_ltc_ops gm20b_ltc_ops = {
	.determine_L2_size_bytes = gm20b_determine_L2_size_bytes,
	.set_max_ways_evict_last = gk20a_ltc_set_max_ways_evict_last,
	.set_zbc_color_entry = gk20a_ltc_set_zbc_color_entry,
	.set_zbc_depth_entry = gk20a_ltc_set_zbc_depth_entry,
	.init_cbc = gk20a_ltc_init_cbc,
#ifdef CONFIG_DEBUG_FS
	.sync_debugfs = gk20a_ltc_sync_debugfs,
#endif
	/* GM20b specific ops. */
	.init_fs_state = gm20b_ltc_init_fs_state,
	.init_comptags = gm20b_ltc_init_comptags,
	.cbc_ctrl = gm20b_ltc_cbc_ctrl,
	.elpg_flush = gm20b_ltc_g_elpg_flush_locked,
	.isr = gm20b_ltc_isr,
	.cbc_fix_config = gm20b_ltc_cbc_fix_config,
	.flush = gm20b_flush_ltc
};

void gm20b_init_ltc(struct gpu_ops *gops)
{
	gops->ltc = &gm20b_ltc_ops;
}
