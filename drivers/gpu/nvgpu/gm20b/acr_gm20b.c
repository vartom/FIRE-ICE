/*
 * GM20B ACR
 *
 * Copyright (c) 2014, NVIDIA CORPORATION.  All rights reserved.
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

#include <linux/delay.h>	/* for mdelay */
#include <linux/firmware.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include "../../../../arch/arm/mach-tegra/iomap.h"

#include "gk20a/gk20a.h"
#include "gk20a/pmu_gk20a.h"
#include "hw_pwr_gm20b.h"
#include "mc_carveout_reg.h"

/*Defines*/
#define gm20b_dbg_pmu(fmt, arg...) \
	gk20a_dbg(gpu_dbg_pmu, fmt, ##arg)
#define GPU_TIMEOUT_DEFAULT 10000

typedef int (*get_ucode_details)(struct gk20a *g, struct flcn_ucode_img *udata);

/*Externs*/

/*Forwards*/
static int lsfm_discover_ucode_images(struct gk20a *g,
	struct ls_flcn_mgr *plsfm);
static int lsfm_add_ucode_img(struct gk20a *g, struct ls_flcn_mgr *plsfm,
	struct flcn_ucode_img *ucode_image, u32 falcon_id);
static void lsfm_free_ucode_img_res(struct flcn_ucode_img *p_img);
static void lsfm_free_nonpmu_ucode_img_res(struct flcn_ucode_img *p_img);
static int lsf_gen_wpr_requirements(struct gk20a *g, struct ls_flcn_mgr *plsfm);
static int lsfm_init_wpr_contents(struct gk20a *g, struct ls_flcn_mgr *plsfm,
	void *nonwpr_addr);
static int acr_ucode_patch_sig(struct gk20a *g,
		unsigned int *p_img,
		unsigned int *p_prod_sig,
		unsigned int *p_dbg_sig,
		unsigned int *p_patch_loc,
		unsigned int *p_patch_ind);

/*Globals*/
static void __iomem *mc = IO_ADDRESS(TEGRA_MC_BASE);
get_ucode_details pmu_acr_supp_ucode_list[MAX_SUPPORTED_LSFM] = {
	pmu_ucode_details,
};

/*Once is LS mode, cpuctl_alias is only accessible*/
void start_gm20b_pmu(struct gk20a *g)
{
	gk20a_writel(g, pwr_falcon_cpuctl_alias_r(),
		pwr_falcon_cpuctl_startcpu_f(1));
}

void gm20b_init_secure_pmu(struct gpu_ops *gops)
{
	gops->pmu.prepare_ucode = prepare_ucode_blob;
	gops->pmu.pmu_setup_hw_and_bootstrap = gm20b_bootstrap_hs_flcn;
}

static void free_blob_res(struct gk20a *g)
{
	/*TODO */
}

int pmu_ucode_details(struct gk20a *g, struct flcn_ucode_img *p_img)
{
	const struct firmware *pmu_fw;
	struct pmu_gk20a *pmu = &g->pmu;
	struct lsf_ucode_desc *lsf_desc;
	int err;
	gm20b_dbg_pmu("requesting PMU ucode in GM20B\n");
	pmu_fw = gk20a_request_firmware(g, GM20B_PMU_UCODE_IMAGE);
	if (!pmu_fw) {
		gk20a_err(dev_from_gk20a(g), "failed to load pmu ucode!!");
		gm20b_dbg_pmu("requesting PMU ucode in GM20B failed\n");
		return -ENOENT;
	}
	gm20b_dbg_pmu("Loaded PMU ucode in for blob preparation");

	pmu->desc = (struct pmu_ucode_desc *)pmu_fw->data;
	pmu->ucode_image = (u32 *)((u8 *)pmu->desc +
			pmu->desc->descriptor_size);
	err = gk20a_init_pmu(pmu);
	if (err) {
		gm20b_dbg_pmu("failed to set function pointers\n");
		return err;
	}

	lsf_desc = kzalloc(sizeof(struct lsf_ucode_desc), GFP_KERNEL);
	if (!lsf_desc)
		return -ENOMEM;
	lsf_desc->falcon_id = LSF_FALCON_ID_PMU;

	p_img->desc = pmu->desc;
	p_img->data = pmu->ucode_image;
	p_img->data_size = pmu->desc->image_size;
	p_img->fw_ver = NULL;
	p_img->header = NULL;
	p_img->lsf_desc = (struct lsf_ucode_desc *)lsf_desc;
	gm20b_dbg_pmu("requesting PMU ucode in GM20B exit\n");
	return 0;
}

int prepare_ucode_blob(struct gk20a *g)
{
	struct device *d = dev_from_gk20a(g);
	dma_addr_t iova;
	u32 status;
	void *nonwpr_addr;
	u64 nonwpr_pmu_va;
	struct ls_flcn_mgr lsfm_l, *plsfm;
	struct sg_table *sgt_nonwpr;
	struct mm_gk20a *mm = &g->mm;
	struct vm_gk20a *vm = &mm->pmu.vm;

	plsfm = &lsfm_l;
	memset((void *)plsfm, 0, sizeof(struct ls_flcn_mgr));
	gm20b_dbg_pmu("fetching GMMU regs\n");
	gm20b_mm_mmu_vpr_info_fetch(g);

	/* Discover all managed falcons*/
	status = lsfm_discover_ucode_images(g, plsfm);
	gm20b_dbg_pmu(" Managed Falcon cnt %d\n", plsfm->managed_flcn_cnt);
	if (status != 0)
		return status;

	if (plsfm->managed_flcn_cnt) {
		/* Generate WPR requirements*/
		status = lsf_gen_wpr_requirements(g, plsfm);
		if (status != 0)
			return status;

		/*Alloc memory to hold ucode blob contents*/
		nonwpr_addr = dma_alloc_coherent(d, plsfm->wpr_size, &iova,
			GFP_KERNEL);
		if (nonwpr_addr == NULL)
			return -ENOMEM;
		status = gk20a_get_sgtable(d, &sgt_nonwpr,
				nonwpr_addr,
				iova,
				plsfm->wpr_size);
		if (status) {
			gk20a_err(d, "failed allocate sg table for nonwpr\n");
			status = -ENOMEM;
			goto err_free_nonwpr_addr;
		}

		nonwpr_pmu_va = gk20a_gmmu_map(vm, &sgt_nonwpr,
				plsfm->wpr_size,
				0, /* flags */
				gk20a_mem_flag_read_only);
		if (!nonwpr_pmu_va) {
			gk20a_err(d, "failed to map pmu ucode memory!!");
			status = -ENOMEM;
			goto err_free_nonwpr_sgt;
		}
		gm20b_dbg_pmu("managed LS falcon %d, WPR size %d bytes.\n",
			plsfm->managed_flcn_cnt, plsfm->wpr_size);
		lsfm_init_wpr_contents(g, plsfm, nonwpr_addr);
		g->acr.ucode_blob_start = nonwpr_pmu_va;
		g->acr.ucode_blob_size = plsfm->wpr_size;
		gm20b_dbg_pmu("32 bit ucode_start %x, size %d\n",
			(u32)nonwpr_pmu_va, plsfm->wpr_size);
		gm20b_dbg_pmu("base reg carveout 2:%x\n",
		readl(mc + MC_SECURITY_CARVEOUT2_BOM_0));
		gm20b_dbg_pmu("base reg carveout 3:%x\n",
		readl(mc + MC_SECURITY_CARVEOUT3_BOM_0));
	} else {
		gm20b_dbg_pmu("LSFM is managing no falcons.\n");
	}
	gm20b_dbg_pmu("prepare ucode blob return 0\n");
	return 0;
err_free_nonwpr_sgt:
	gk20a_free_sgtable(&sgt_nonwpr);
err_free_nonwpr_addr:
	dma_free_coherent(d, plsfm->wpr_size,
			nonwpr_addr, iova);
	nonwpr_addr = NULL;
	iova = 0;
	gm20b_dbg_pmu("prepare ucode blob return %x\n", status);
	return status;
}

u8 lsfm_falcon_disabled(struct gk20a *g, struct ls_flcn_mgr *plsfm,
	u32 falcon_id)
{
	return (plsfm->disable_mask >> falcon_id) & 0x1;
}

/* Discover all managed falcon ucode images */
static int lsfm_discover_ucode_images(struct gk20a *g,
	struct ls_flcn_mgr *plsfm)
{
	struct pmu_gk20a *pmu = &g->pmu;
	struct flcn_ucode_img ucode_img;
	u32 falcon_id;
	u32 i;
	int status;

	/* LSFM requires a secure PMU, discover it first.*/
	/* Obtain the PMU ucode image and add it to the list if required*/
	memset(&ucode_img, 0, sizeof(ucode_img));
	status = pmu_ucode_details(g, &ucode_img);
	if (status == 0) {
		if (ucode_img.lsf_desc != NULL) {
			/* The falonId is formed by grabbing the static base
			 * falonId from the image and adding the
			 * engine-designated falcon instance.*/
			pmu->pmu_mode |= PMU_SECURE_MODE;
			falcon_id = ucode_img.lsf_desc->falcon_id +
				ucode_img.flcn_inst;

			if (!lsfm_falcon_disabled(g, plsfm, falcon_id)) {
				pmu->falcon_id = falcon_id;
				if (lsfm_add_ucode_img(g, plsfm, &ucode_img,
					pmu->falcon_id) == 0)
					pmu->pmu_mode |= PMU_LSFM_MANAGED;

				plsfm->managed_flcn_cnt++;
			} else {
				gm20b_dbg_pmu("id not managed %d\n",
					ucode_img.lsf_desc->falcon_id);
			}
		}

		/*Free any ucode image resources if not managing this falcon*/
		if (!(pmu->pmu_mode & PMU_LSFM_MANAGED)) {
			gm20b_dbg_pmu("pmu is not LSFM managed\n");
			lsfm_free_ucode_img_res(&ucode_img);
		}
	}

	/* Enumerate all constructed falcon objects,
	 as we need the ucode image info and total falcon count.*/

	/*0th index is always PMU which is already handled in earlier
	if condition*/
	for (i = 1; i < MAX_SUPPORTED_LSFM; i++) {
		memset(&ucode_img, 0, sizeof(ucode_img));
		if (pmu_acr_supp_ucode_list[i](g, &ucode_img) == 0) {
			if (ucode_img.lsf_desc != NULL) {
				/* We have engine sigs, ensure that this falcon
				is aware of the secure mode expectations
				(ACR status)*/

				/* falon_id is formed by grabbing the static
				base falonId from the image and adding the
				engine-designated falcon instance. */
				falcon_id = ucode_img.lsf_desc->falcon_id +
					ucode_img.flcn_inst;

				if (!lsfm_falcon_disabled(g, plsfm,
					falcon_id)) {
					/* Do not manage non-FB ucode*/
					if (lsfm_add_ucode_img(g,
						plsfm, &ucode_img, falcon_id)
						== 0)
						plsfm->managed_flcn_cnt++;
				} else {
					gm20b_dbg_pmu("not managed %d\n",
						ucode_img.lsf_desc->falcon_id);
					lsfm_free_nonpmu_ucode_img_res(
						&ucode_img);
				}
			}
		} else {
			/* Consumed all available falcon objects */
			gm20b_dbg_pmu("Done checking for ucodes %d\n", i);
			break;
		}
	}
	return 0;
}


int pmu_populate_loader_cfg(struct gk20a *g,
	struct lsfm_managed_ucode_img *lsfm,
	union flcn_bl_generic_desc *p_bl_gen_desc, u32 *p_bl_gen_desc_size)
{
	struct pmu_gk20a *pmu = &g->pmu;
	struct flcn_ucode_img *p_img = &(lsfm->ucode_img);
	struct loader_config *ldr_cfg =
		(struct loader_config *)(&p_bl_gen_desc->loader_cfg);
	struct gk20a_platform *platform = platform_get_drvdata(g->dev);
	u64 addr_base;
	struct pmu_ucode_desc *desc;
	u64 addr_code, addr_data;
	u32 addr_args;

	if (p_img->desc == NULL) /*This means its a header based ucode,
				  and so we do not fill BL gen desc structure*/
		return -EINVAL;
	desc = p_img->desc;
	/*
	 Calculate physical and virtual addresses for various portions of
	 the PMU ucode image
	 Calculate the 32-bit addresses for the application code, application
	 data, and bootloader code. These values are all based on IM_BASE.
	 The 32-bit addresses will be the upper 32-bits of the virtual or
	 physical addresses of each respective segment.
	*/
	addr_base = lsfm->lsb_header.ucode_off;
	addr_base += readl(mc + MC_SECURITY_CARVEOUT3_BOM_0);
	gm20b_dbg_pmu("pmu loader cfg u32 addrbase %x\n", (u32)addr_base);
	/*From linux*/
	addr_code = u64_lo32((addr_base +
				desc->app_start_offset +
				desc->app_resident_code_offset) >> 8);
	gm20b_dbg_pmu("app start %d app res code off %d\n",
		desc->app_start_offset, desc->app_resident_code_offset);
	addr_data = u64_lo32((addr_base +
				desc->app_start_offset +
				desc->app_resident_data_offset) >> 8);
	gm20b_dbg_pmu("app res data offset%d\n",
		desc->app_resident_data_offset);
	gm20b_dbg_pmu("bl start off %d\n", desc->bootloader_start_offset);

	addr_args = ((pwr_falcon_hwcfg_dmem_size_v(
			gk20a_readl(g, pwr_falcon_hwcfg_r())))
			<< GK20A_PMU_DMEM_BLKSIZE2);
	addr_args -= g->ops.pmu_ver.get_pmu_cmdline_args_size(pmu);

	gm20b_dbg_pmu("addr_args %x\n", addr_args);

	/* Populate the loader_config state*/
	ldr_cfg->dma_idx = 2;
	ldr_cfg->code_dma_base = addr_code;
	ldr_cfg->code_size_total = desc->app_size;
	ldr_cfg->code_size_to_load = desc->app_resident_code_size;
	ldr_cfg->code_entry_point = desc->app_imem_entry;
	ldr_cfg->data_dma_base = addr_data;
	ldr_cfg->data_size = desc->app_resident_data_size;
	ldr_cfg->overlay_dma_base = addr_code;

	/* Update the argc/argv members*/
	ldr_cfg->argc = 1;
	ldr_cfg->argv = addr_args;

	/*Copying pmu cmdline args*/
	g->ops.pmu_ver.set_pmu_cmdline_args_cpu_freq(pmu,
				clk_get_rate(platform->clk[1]));
	g->ops.pmu_ver.set_pmu_cmdline_args_secure_mode(pmu, 1);
	pmu_copy_to_dmem(pmu, addr_args,
			(u8 *)(g->ops.pmu_ver.get_pmu_cmdline_args_ptr(pmu)),
			g->ops.pmu_ver.get_pmu_cmdline_args_size(pmu), 0);
	*p_bl_gen_desc_size = sizeof(p_bl_gen_desc->loader_cfg);
	return 0;
}

int flcn_populate_bl_dmem_desc(struct gk20a *g,
	struct lsfm_managed_ucode_img *lsfm,
	union flcn_bl_generic_desc *p_bl_gen_desc, u32 *p_bl_gen_desc_size)
{

	struct flcn_ucode_img *p_img = &(lsfm->ucode_img);
	struct flcn_bl_dmem_desc *ldr_cfg =
		(struct flcn_bl_dmem_desc *)(&p_bl_gen_desc->loader_cfg);
	u64 addr_base;
	struct pmu_ucode_desc *desc;
	u64 addr_code, addr_data;

	if (p_img->desc == NULL) /*This means its a header based ucode,
				  and so we do not fill BL gen desc structure*/
		return -EINVAL;
	desc = p_img->desc;

	/*
	 Calculate physical and virtual addresses for various portions of
	 the PMU ucode image
	 Calculate the 32-bit addresses for the application code, application
	 data, and bootloader code. These values are all based on IM_BASE.
	 The 32-bit addresses will be the upper 32-bits of the virtual or
	 physical addresses of each respective segment.
	*/
	addr_base = lsfm->lsb_header.ucode_off;
	addr_base += readl(mc + MC_SECURITY_CARVEOUT3_BOM_0);
	gm20b_dbg_pmu("gen loader cfg %x u32 addrbase %x ID\n", (u32)addr_base,
		lsfm->wpr_header.falcon_id);
	addr_code = u64_lo32((addr_base +
				desc->app_start_offset +
				desc->app_resident_code_offset) >> 8);
	addr_data = u64_lo32((addr_base +
				desc->app_start_offset +
				desc->app_resident_data_offset) >> 8);

	gm20b_dbg_pmu("gen cfg %x u32 addrcode %x & data %x load offset %xID\n",
		(u32)addr_code, (u32)addr_data, desc->bootloader_start_offset,
		lsfm->wpr_header.falcon_id);

	/* Populate the LOADER_CONFIG state */
	memset((void *) ldr_cfg, 0, sizeof(struct flcn_bl_dmem_desc));
	ldr_cfg->ctx_dma = 0;
	ldr_cfg->code_dma_base = addr_code;
	ldr_cfg->non_sec_code_size = desc->app_resident_code_size;
	ldr_cfg->data_dma_base = addr_data;
	ldr_cfg->data_size = desc->app_resident_data_size;
	ldr_cfg->code_entry_point = desc->app_imem_entry;
	*p_bl_gen_desc_size = sizeof(p_bl_gen_desc->bl_dmem_desc);
	return 0;
}

/* Populate falcon boot loader generic desc.*/
static int lsfm_fill_flcn_bl_gen_desc(struct gk20a *g,
		struct lsfm_managed_ucode_img *pnode)
{

	struct pmu_gk20a *pmu = &g->pmu;
	if (pnode->wpr_header.falcon_id != pmu->falcon_id) {
		gm20b_dbg_pmu("non pmu. write flcn bl gen desc\n");
		flcn_populate_bl_dmem_desc(g, pnode, &pnode->bl_gen_desc,
				&pnode->bl_gen_desc_size);
		return 0;
	}

	if (pmu->pmu_mode & PMU_LSFM_MANAGED) {
		gm20b_dbg_pmu("pmu write flcn bl gen desc\n");
		if (pnode->wpr_header.falcon_id == pmu->falcon_id)
			return pmu_populate_loader_cfg(g, pnode,
				&pnode->bl_gen_desc, &pnode->bl_gen_desc_size);
	}

	/* Failed to find the falcon requested. */
	return -ENOENT;
}

/* Initialize WPR contents */
static int lsfm_init_wpr_contents(struct gk20a *g, struct ls_flcn_mgr *plsfm,
	void *nonwpr_addr)
{

	int status = 0;
	union flcn_bl_generic_desc *nonwpr_bl_gen_desc;
	if (nonwpr_addr == NULL) {
		status = -ENOMEM;
	} else {
		struct lsfm_managed_ucode_img *pnode = plsfm->ucode_img_list;
		struct lsf_wpr_header *wpr_hdr;
		struct lsf_lsb_header *lsb_hdr;
		void *ucode_off;
		u32 i;

		/* The WPR array is at the base of the WPR */
		wpr_hdr = (struct lsf_wpr_header *)nonwpr_addr;
		pnode = plsfm->ucode_img_list;
		i = 0;

		/*
		 * Walk the managed falcons, flush WPR and LSB headers to FB.
		 * flush any bl args to the storage area relative to the
		 * ucode image (appended on the end as a DMEM area).
		 */
		while (pnode) {
			/* Flush WPR header to memory*/
			memcpy(&wpr_hdr[i], &pnode->wpr_header,
					sizeof(struct lsf_wpr_header));
			gm20b_dbg_pmu("wpr header as in memory and pnode\n");
			gm20b_dbg_pmu("falconid :%d %d\n",
				pnode->wpr_header.falcon_id,
				wpr_hdr[i].falcon_id);
			gm20b_dbg_pmu("lsb_offset :%x %x\n",
				pnode->wpr_header.lsb_offset,
				wpr_hdr[i].lsb_offset);
			gm20b_dbg_pmu("bootstrap_owner :%d %d\n",
				pnode->wpr_header.bootstrap_owner,
				wpr_hdr[i].bootstrap_owner);
			gm20b_dbg_pmu("lazy_bootstrap :%d %d\n",
				pnode->wpr_header.lazy_bootstrap,
				wpr_hdr[i].lazy_bootstrap);
			gm20b_dbg_pmu("status :%d %d\n",
				pnode->wpr_header.status, wpr_hdr[i].status);

			/*Flush LSB header to memory*/
			lsb_hdr = (struct lsf_lsb_header *)((u8 *)nonwpr_addr +
					pnode->wpr_header.lsb_offset);
			memcpy(lsb_hdr, &pnode->lsb_header,
					sizeof(struct lsf_lsb_header));
			gm20b_dbg_pmu("lsb header as in memory and pnode\n");
			gm20b_dbg_pmu("ucode_off :%x %x\n",
				pnode->lsb_header.ucode_off,
				lsb_hdr->ucode_off);
			gm20b_dbg_pmu("ucode_size :%x %x\n",
				pnode->lsb_header.ucode_size,
				lsb_hdr->ucode_size);
			gm20b_dbg_pmu("data_size :%x %x\n",
				pnode->lsb_header.data_size,
				lsb_hdr->data_size);
			gm20b_dbg_pmu("bl_code_size :%x %x\n",
				pnode->lsb_header.bl_code_size,
				lsb_hdr->bl_code_size);
			gm20b_dbg_pmu("bl_imem_off :%x %x\n",
				pnode->lsb_header.bl_imem_off,
				lsb_hdr->bl_imem_off);
			gm20b_dbg_pmu("bl_data_off :%x %x\n",
				pnode->lsb_header.bl_data_off,
				lsb_hdr->bl_data_off);
			gm20b_dbg_pmu("bl_data_size :%x %x\n",
				pnode->lsb_header.bl_data_size,
				lsb_hdr->bl_data_size);
			gm20b_dbg_pmu("flags :%x %x\n",
				pnode->lsb_header.flags, lsb_hdr->flags);

			/*If this falcon has a boot loader and related args,
			 * flush them.*/
			if (!pnode->ucode_img.header) {
				nonwpr_bl_gen_desc =
					(union flcn_bl_generic_desc *)
					((u8 *)nonwpr_addr +
					pnode->lsb_header.bl_data_off);

				/*Populate gen bl and flush to memory*/
				lsfm_fill_flcn_bl_gen_desc(g, pnode);
				memcpy(nonwpr_bl_gen_desc, &pnode->bl_gen_desc,
					pnode->bl_gen_desc_size);
			}
			ucode_off = (void *)(pnode->lsb_header.ucode_off +
				(u8 *)nonwpr_addr);
			/*Copying of ucode*/
			memcpy(ucode_off, pnode->ucode_img.data,
				pnode->ucode_img.data_size);
			pnode = pnode->next;
			i++;
		}

		/* Tag the terminator WPR header with an invalid falcon ID. */
		gk20a_mem_wr32(&wpr_hdr[plsfm->managed_flcn_cnt].falcon_id,
			1, LSF_FALCON_ID_INVALID);
	}
	return status;
}

/*!
 * lsfm_parse_no_loader_ucode: parses UCODE header of falcon
 *
 * @param[in] p_ucodehdr : UCODE header
 * @param[out] lsb_hdr : updates values in LSB header
 *
 * @return 0
 */
static int lsfm_parse_no_loader_ucode(u32 *p_ucodehdr,
	struct lsf_lsb_header *lsb_hdr)
{

	u32 code_size = 0;
	u32 data_size = 0;
	u32 i = 0;
	u32 total_apps = p_ucodehdr[FLCN_NL_UCODE_HDR_NUM_APPS_IND];

	/* Lets calculate code size*/
	code_size += p_ucodehdr[FLCN_NL_UCODE_HDR_OS_CODE_SIZE_IND];
	for (i = 0; i < total_apps; i++) {
		code_size += p_ucodehdr[FLCN_NL_UCODE_HDR_APP_CODE_SIZE_IND
			(total_apps, i)];
	}
	code_size += p_ucodehdr[FLCN_NL_UCODE_HDR_OS_OVL_SIZE_IND(total_apps)];

	/* Calculate data size*/
	data_size += p_ucodehdr[FLCN_NL_UCODE_HDR_OS_DATA_SIZE_IND];
	for (i = 0; i < total_apps; i++) {
		data_size += p_ucodehdr[FLCN_NL_UCODE_HDR_APP_DATA_SIZE_IND
			(total_apps, i)];
	}

	lsb_hdr->ucode_size = code_size;
	lsb_hdr->data_size = data_size;
	lsb_hdr->bl_code_size = p_ucodehdr[FLCN_NL_UCODE_HDR_OS_CODE_SIZE_IND];
	lsb_hdr->bl_imem_off = 0;
	lsb_hdr->bl_data_off = p_ucodehdr[FLCN_NL_UCODE_HDR_OS_DATA_OFF_IND];
	lsb_hdr->bl_data_size = p_ucodehdr[FLCN_NL_UCODE_HDR_OS_DATA_SIZE_IND];
	return 0;
}

/*!
 * @brief lsfm_fill_static_lsb_hdr_info
 * Populate static LSB header infomation using the provided ucode image
 */
static void lsfm_fill_static_lsb_hdr_info(struct gk20a *g,
	u32 falcon_id, struct lsfm_managed_ucode_img *pnode)
{

	struct pmu_gk20a *pmu = &g->pmu;
	u32 data = 0;

	if (pnode->ucode_img.lsf_desc)
		memcpy(&pnode->lsb_header.signature, pnode->ucode_img.lsf_desc,
			sizeof(struct lsf_ucode_desc));
	pnode->lsb_header.ucode_size = pnode->ucode_img.data_size;

	/* The remainder of the LSB depends on the loader usage */
	if (pnode->ucode_img.header) {
		/* Does not use a loader */
		pnode->lsb_header.data_size = 0;
		pnode->lsb_header.bl_code_size = 0;
		pnode->lsb_header.bl_data_off = 0;
		pnode->lsb_header.bl_data_size = 0;

		lsfm_parse_no_loader_ucode(pnode->ucode_img.header,
			&(pnode->lsb_header));

		/* Load the first 256 bytes of IMEM. */
		/* Set LOAD_CODE_AT_0 and DMACTL_REQ_CTX.
		True for all method based falcons */
		data = NV_FLCN_ACR_LSF_FLAG_LOAD_CODE_AT_0_TRUE |
			NV_FLCN_ACR_LSF_FLAG_DMACTL_REQ_CTX_TRUE;
		pnode->lsb_header.flags = data;
	} else {
		/* Uses a loader. that is has a desc */
		pnode->lsb_header.data_size = 0;

		/* The loader code size is already aligned (padded) such that
		the code following it is aligned, but the size in the image
		desc is not, bloat it up to be on a 256 byte alignment. */
		pnode->lsb_header.bl_code_size = ALIGN(
			pnode->ucode_img.desc->bootloader_size,
			LSF_BL_CODE_SIZE_ALIGNMENT);
		/* Though the BL is located at 0th offset of the image, the VA
		is different to make sure that it doesnt collide the actual OS
		VA range */
		pnode->lsb_header.bl_imem_off =
			pnode->ucode_img.desc->bootloader_imem_offset;

		/* TODO: OBJFLCN should export properties using which the below
			flags should be populated.*/
		pnode->lsb_header.flags = 0;

		if (falcon_id == pmu->falcon_id) {
			data = NV_FLCN_ACR_LSF_FLAG_DMACTL_REQ_CTX_TRUE;
			pnode->lsb_header.flags = data;
		}
	}
}

/* Adds a ucode image to the list of managed ucode images managed. */
static int lsfm_add_ucode_img(struct gk20a *g, struct ls_flcn_mgr *plsfm,
	struct flcn_ucode_img *ucode_image, u32 falcon_id)
{

	struct lsfm_managed_ucode_img *pnode;
	pnode = kzalloc(sizeof(struct lsfm_managed_ucode_img), GFP_KERNEL);
	if (pnode == NULL)
		return -ENOMEM;

	/* Keep a copy of the ucode image info locally */
	memcpy(&pnode->ucode_img, ucode_image, sizeof(struct flcn_ucode_img));

	/* Fill in static WPR header info*/
	pnode->wpr_header.falcon_id = falcon_id;
	pnode->wpr_header.bootstrap_owner = LSF_BOOTSTRAP_OWNER_DEFAULT;
	pnode->wpr_header.status = LSF_IMAGE_STATUS_COPY;

	/*TODO to check if PDB_PROP_FLCN_LAZY_BOOTSTRAP is to be supported by
	Android */
	/* Fill in static LSB header info elsewhere */
	lsfm_fill_static_lsb_hdr_info(g, falcon_id, pnode);
	pnode->next = plsfm->ucode_img_list;
	plsfm->ucode_img_list = pnode;
	return 0;
}

/* Free any ucode image structure resources*/
static void lsfm_free_ucode_img_res(struct flcn_ucode_img *p_img)
{
	if (p_img->lsf_desc != NULL) {
		kfree(p_img->lsf_desc);
		p_img->lsf_desc = NULL;
	}
}

/* Free any ucode image structure resources*/
static void lsfm_free_nonpmu_ucode_img_res(struct flcn_ucode_img *p_img)
{
	if (p_img->lsf_desc != NULL) {
		kfree(p_img->lsf_desc);
		p_img->lsf_desc = NULL;
	}
	if (p_img->desc != NULL) {
		kfree(p_img->desc);
		p_img->desc = NULL;
	}
}


/* Generate WPR requirements for ACR allocation request */
static int lsf_gen_wpr_requirements(struct gk20a *g, struct ls_flcn_mgr *plsfm)
{
	struct lsfm_managed_ucode_img *pnode = plsfm->ucode_img_list;
	u32 wpr_offset;

	/* Calculate WPR size required */

	/* Start with an array of WPR headers at the base of the WPR.
	 The expectation here is that the secure falcon will do a single DMA
	 read of this array and cache it internally so it's OK to pack these.
	 Also, we add 1 to the falcon count to indicate the end of the array.*/
	wpr_offset = sizeof(struct lsf_wpr_header) *
		(plsfm->managed_flcn_cnt+1);

	/* Walk the managed falcons, accounting for the LSB structs
	as well as the ucode images. */
	while (pnode) {
		/* Align, save off, and include an LSB header size */
		wpr_offset = ALIGN(wpr_offset,
			LSF_LSB_HEADER_ALIGNMENT);
		pnode->wpr_header.lsb_offset = wpr_offset;
		wpr_offset += sizeof(struct lsf_lsb_header);

		/* Align, save off, and include the original (static)
		ucode image size */
		wpr_offset = ALIGN(wpr_offset,
			LSF_UCODE_DATA_ALIGNMENT);
		pnode->lsb_header.ucode_off = wpr_offset;
		wpr_offset += pnode->ucode_img.data_size;

		/* For falcons that use a boot loader (BL), we append a loader
		desc structure on the end of the ucode image and consider this
		the boot loader data. The host will then copy the loader desc
		args to this space within the WPR region (before locking down)
		and the HS bin will then copy them to DMEM 0 for the loader. */
		if (!pnode->ucode_img.header) {
			/* Track the size for LSB details filled in later
			 Note that at this point we don't know what kind of i
			boot loader desc, so we just take the size of the
			generic one, which is the largest it will will ever be.
			*/
			/* Align (size bloat) and save off generic
			descriptor size*/
			pnode->lsb_header.bl_data_size = ALIGN(
				sizeof(pnode->bl_gen_desc),
				LSF_BL_DATA_SIZE_ALIGNMENT);

			/*Align, save off, and include the additional BL data*/
			wpr_offset = ALIGN(wpr_offset,
				LSF_BL_DATA_ALIGNMENT);
			pnode->lsb_header.bl_data_off = wpr_offset;
			wpr_offset += pnode->lsb_header.bl_data_size;
		} else {
			/* bl_data_off is already assigned in static
			information. But that is from start of the image */
			pnode->lsb_header.bl_data_off +=
				(wpr_offset - pnode->ucode_img.data_size);
		}

		/* Finally, update ucode surface size to include updates */
		pnode->full_ucode_size = wpr_offset -
			pnode->lsb_header.ucode_off;
		pnode = pnode->next;
	}
	plsfm->wpr_size = wpr_offset;
	return 0;
}

/*Loads ACR bin to FB mem and bootstraps PMU with bootloader code
 * start and end are addresses of ucode blob in non-WPR region*/
int gm20b_bootstrap_hs_flcn(struct gk20a *g)
{
	struct mm_gk20a *mm = &g->mm;
	struct vm_gk20a *vm = &mm->pmu.vm;
	struct device *d = dev_from_gk20a(g);
	int i, err = 0;
	struct sg_table *sgt_pmu_ucode;
	dma_addr_t iova;
	u64 *pacr_ucode_cpuva = NULL, pacr_ucode_pmu_va, *acr_dmem;
	u32 img_size_in_bytes;
	struct flcn_bl_dmem_desc bl_dmem_desc;
	u32 status, start, size;
	const struct firmware *acr_fw;
	struct acr_gm20b *acr = &g->acr;
	u32 *acr_ucode_header_t210_load;
	u32 *acr_ucode_data_t210_load;

	start = g->acr.ucode_blob_start;
	size = g->acr.ucode_blob_size;

	gm20b_dbg_pmu("");

	acr_fw = gk20a_request_firmware(g, GM20B_HSBIN_PMU_UCODE_IMAGE);
	if (!acr_fw) {
		gk20a_err(dev_from_gk20a(g), "failed to load pmu ucode!!");
		return -ENOENT;
	}
	acr->hsbin_hdr = (struct bin_hdr *)acr_fw->data;
	acr->fw_hdr = (struct acr_fw_header *)(acr_fw->data +
		acr->hsbin_hdr->header_offset);
	acr_ucode_data_t210_load = (u32 *)(acr_fw->data +
		acr->hsbin_hdr->data_offset);
	acr_ucode_header_t210_load = (u32 *)(acr_fw->data +
		acr->fw_hdr->hdr_offset);
	img_size_in_bytes = ALIGN((acr->hsbin_hdr->data_size), 256);

	/* Lets patch the signatures first.. */
	if (acr_ucode_patch_sig(g, acr_ucode_data_t210_load,
		(u32 *)(acr_fw->data + acr->fw_hdr->sig_prod_offset),
		(u32 *)(acr_fw->data + acr->fw_hdr->sig_dbg_offset),
		(u32 *)(acr_fw->data + acr->fw_hdr->patch_loc),
		(u32 *)(acr_fw->data + acr->fw_hdr->patch_sig)) < 0)
		return -1;
	pacr_ucode_cpuva = dma_alloc_coherent(d, img_size_in_bytes, &iova,
			GFP_KERNEL);
	if (!pacr_ucode_cpuva)
		return -ENOMEM;

	err = gk20a_get_sgtable(d, &sgt_pmu_ucode,
				pacr_ucode_cpuva,
				iova,
				img_size_in_bytes);
	if (err) {
		gk20a_err(d, "failed to allocate sg table\n");
		err = -ENOMEM;
		goto err_free_acr_buf;
	}
	pacr_ucode_pmu_va = gk20a_gmmu_map(vm, &sgt_pmu_ucode,
					img_size_in_bytes,
					0, /* flags */
					gk20a_mem_flag_read_only);
	if (!pacr_ucode_pmu_va) {
		gk20a_err(d, "failed to map pmu ucode memory!!");
		err = -ENOMEM;
		goto err_free_ucode_sgt;
	}
	acr_dmem = (u64 *)
		&(((u8 *)acr_ucode_data_t210_load)[
		acr_ucode_header_t210_load[2]]);
	((struct flcn_acr_desc *)acr_dmem)->nonwpr_ucode_blob_start =
		start;
	((struct flcn_acr_desc *)acr_dmem)->nonwpr_ucode_blob_size =
		size;
	((struct flcn_acr_desc *)acr_dmem)->wpr_region_id = 2;
	((struct flcn_acr_desc *)acr_dmem)->regions.no_regions = 2;
	((struct flcn_acr_desc *)acr_dmem)->regions.region_props[0].region_id
								= 2;
	((struct flcn_acr_desc *)acr_dmem)->regions.region_props[1].region_id
								= 3;
	((struct flcn_acr_desc *)acr_dmem)->wpr_offset = 0;

	for (i = 0; i < (img_size_in_bytes/4); i++) {
		gk20a_mem_wr32(pacr_ucode_cpuva, i,
			acr_ucode_data_t210_load[i]);
	}
	/*
	 * In order to execute this binary, we will be using PMU HAL to run
	 * a bootloader which will load this image into PMU IMEM/DMEM.
	 * Fill up the bootloader descriptor for PMU HAL to use..
	 * TODO: Use standard descriptor which the generic bootloader is
	 * checked in.
	 */

	bl_dmem_desc.signature[0] = 0;
	bl_dmem_desc.signature[1] = 0;
	bl_dmem_desc.signature[2] = 0;
	bl_dmem_desc.signature[3] = 0;
	bl_dmem_desc.ctx_dma = GK20A_PMU_DMAIDX_UCODE;
	bl_dmem_desc.code_dma_base =
		(unsigned int)(((u64)pacr_ucode_pmu_va >> 8));
	bl_dmem_desc.non_sec_code_off  = acr_ucode_header_t210_load[0];
	bl_dmem_desc.non_sec_code_size = acr_ucode_header_t210_load[1];
	bl_dmem_desc.sec_code_off = acr_ucode_header_t210_load[5];
	bl_dmem_desc.sec_code_size = acr_ucode_header_t210_load[6];
	bl_dmem_desc.code_entry_point = 0; /* Start at 0th offset */
	bl_dmem_desc.data_dma_base =
				bl_dmem_desc.code_dma_base +
				((acr_ucode_header_t210_load[2]) >> 8);
	bl_dmem_desc.data_size = acr_ucode_header_t210_load[3];
	status = pmu_exec_gen_bl(g, &bl_dmem_desc, 1);
	if (status != 0) {
		err = status;
		goto err_free_ucode_map;
	}
	return 0;
err_free_ucode_map:
	gk20a_gmmu_unmap(vm, pacr_ucode_pmu_va,
			img_size_in_bytes, gk20a_mem_flag_none);
err_free_ucode_sgt:
	gk20a_free_sgtable(&sgt_pmu_ucode);
err_free_acr_buf:
	dma_free_coherent(d, img_size_in_bytes,
		pacr_ucode_cpuva, iova);
	return err;
}

u8 pmu_is_debug_mode_en(struct gk20a *g)
{
	int ctl_stat =  gk20a_readl(g, pwr_pmu_scpctl_stat_r());
	return 1;
/*TODO return (ctl_stat & pwr_pmu_scpctl_stat_debug_mode_m());*/
}

/*
 * @brief Patch signatures into ucode image
 */
static int
acr_ucode_patch_sig(struct gk20a *g,
		unsigned int *p_img,
		unsigned int *p_prod_sig,
		unsigned int *p_dbg_sig,
		unsigned int *p_patch_loc,
		unsigned int *p_patch_ind)
{
	int i, *p_sig;
	gm20b_dbg_pmu("");

	if (!pmu_is_debug_mode_en(g)) {
		p_sig = p_prod_sig;
		gm20b_dbg_pmu("PRODUCTION MODE\n");
	} else {
		p_sig = p_dbg_sig;
		gm20b_dbg_pmu("DEBUG MODE\n");
	}

	/* Patching logic:*/
	for (i = 0; i < sizeof(*p_patch_loc)>>2; i++) {
		p_img[(p_patch_loc[i]>>2)] = p_sig[(p_patch_ind[i]<<2)];
		p_img[(p_patch_loc[i]>>2)+1] = p_sig[(p_patch_ind[i]<<2)+1];
		p_img[(p_patch_loc[i]>>2)+2] = p_sig[(p_patch_ind[i]<<2)+2];
		p_img[(p_patch_loc[i]>>2)+3] = p_sig[(p_patch_ind[i]<<2)+3];
	}
	return 0;
}

static int bl_bootstrap(struct pmu_gk20a *pmu,
	struct flcn_bl_dmem_desc *pbl_desc, u32 bl_sz)
{
	struct gk20a *g = gk20a_from_pmu(pmu);
	struct mm_gk20a *mm = &g->mm;
	struct pmu_ucode_desc *desc = pmu->desc;
	u32 imem_dst_blk = 0;
	u32 virt_addr = 0;
	u32 tag = 0;
	u32 index = 0;
	struct hsflcn_bl_desc *pmu_bl_gm10x_desc = g->acr.pmu_hsbl_desc;
	u32 *bl_ucode;

	gk20a_dbg_fn("");
	gk20a_writel(g, pwr_falcon_itfen_r(),
			gk20a_readl(g, pwr_falcon_itfen_r()) |
			pwr_falcon_itfen_ctxen_enable_f());
	gk20a_writel(g, pwr_pmu_new_instblk_r(),
			pwr_pmu_new_instblk_ptr_f(
				mm->pmu.inst_block.cpu_pa >> 12) |
			pwr_pmu_new_instblk_valid_f(1) |
			pwr_pmu_new_instblk_target_sys_coh_f());

	/* TBD: load all other surfaces */
	/*copy bootloader interface structure to dmem*/
	gk20a_writel(g, pwr_falcon_dmemc_r(0),
			pwr_falcon_dmemc_offs_f(0) |
			pwr_falcon_dmemc_blk_f(0)  |
			pwr_falcon_dmemc_aincw_f(1));
	pmu_copy_to_dmem(pmu, 0, (u8 *)pbl_desc,
		sizeof(struct flcn_bl_dmem_desc), 0);
	/*TODO This had to be copied to bl_desc_dmem_load_off, but since
	 * this is 0, so ok for now*/

	/* Now copy bootloader to TOP of IMEM */
	imem_dst_blk = (pwr_falcon_hwcfg_imem_size_v(
			gk20a_readl(g, pwr_falcon_hwcfg_r()))) - bl_sz/256;

	/* Set Auto-Increment on write */
	gk20a_writel(g, pwr_falcon_imemc_r(0),
			pwr_falcon_imemc_offs_f(0) |
			pwr_falcon_imemc_blk_f(imem_dst_blk)  |
			pwr_falcon_imemc_aincw_f(1));
	virt_addr = pmu_bl_gm10x_desc->bl_start_tag << 8;
	tag = virt_addr >> 8; /* tag is always 256B aligned */
	bl_ucode = (u32 *)(pmu->ucode.cpuva);
	for (index = 0; index < bl_sz/4; index++) {
		if ((index % 64) == 0) {
			gk20a_writel(g, pwr_falcon_imemt_r(0),
				(tag & 0xffff) << 0);
			tag++;
		}
		gk20a_writel(g, pwr_falcon_imemd_r(0),
				bl_ucode[index] & 0xffffffff);
	}

	gk20a_writel(g, pwr_falcon_imemt_r(0), (0 & 0xffff) << 0);
	gm20b_dbg_pmu("Before starting falcon with BL\n");

	gk20a_writel(g, pwr_falcon_bootvec_r(),
			pwr_falcon_bootvec_vec_f(virt_addr));

	gk20a_writel(g, pwr_falcon_cpuctl_r(),
			pwr_falcon_cpuctl_startcpu_f(1));

	gk20a_writel(g, pwr_falcon_os_r(), desc->app_version);

	return 0;
}

int gm20b_init_pmu_setup_hw1(struct gk20a *g, struct flcn_bl_dmem_desc *desc,
		u32 bl_sz)
{
	struct pmu_gk20a *pmu = &g->pmu;
	int err;

	gk20a_dbg_fn("");
	pmu_reset(pmu);

	/* setup apertures - virtual */
	gk20a_writel(g, pwr_fbif_transcfg_r(GK20A_PMU_DMAIDX_UCODE),
			pwr_fbif_transcfg_mem_type_virtual_f());
	gk20a_writel(g, pwr_fbif_transcfg_r(GK20A_PMU_DMAIDX_VIRT),
			pwr_fbif_transcfg_mem_type_virtual_f());
	/* setup apertures - physical */
	gk20a_writel(g, pwr_fbif_transcfg_r(GK20A_PMU_DMAIDX_PHYS_VID),
			pwr_fbif_transcfg_mem_type_physical_f() |
			pwr_fbif_transcfg_target_local_fb_f());
	gk20a_writel(g, pwr_fbif_transcfg_r(GK20A_PMU_DMAIDX_PHYS_SYS_COH),
			pwr_fbif_transcfg_mem_type_physical_f() |
			pwr_fbif_transcfg_target_coherent_sysmem_f());
	gk20a_writel(g, pwr_fbif_transcfg_r(GK20A_PMU_DMAIDX_PHYS_SYS_NCOH),
			pwr_fbif_transcfg_mem_type_physical_f() |
			pwr_fbif_transcfg_target_noncoherent_sysmem_f());

	err = bl_bootstrap(pmu, desc, bl_sz);
	if (err)
		return err;
	return 0;
}

/*
* Executes a generic bootloader and wait for PMU to halt.
* This BL will be used for those binaries that are loaded
* and executed at times other than RM PMU Binary execution.
*
* @param[in] g			gk20a pointer
* @param[in] desc		Bootloader descriptor
* @param[in] dma_idx		DMA Index
* @param[in] b_wait_for_halt	Wait for PMU to HALT
*/
int pmu_exec_gen_bl(struct gk20a *g, void *desc, u8 b_wait_for_halt)
{
	struct pmu_gk20a *pmu = &g->pmu;
	struct mm_gk20a *mm = &g->mm;
	struct vm_gk20a *vm = &mm->pmu.vm;
	struct device *d = dev_from_gk20a(g);
	int i, err = 0;
	struct sg_table *sgt_pmu_ucode;
	dma_addr_t iova;
	u32 bl_sz;
	void *bl_cpuva;
	u64 bl_pmu_va;
	const struct firmware *hsbl_fw;
	struct acr_gm20b *acr = &g->acr;
	struct hsflcn_bl_desc *pmu_bl_gm10x_desc;
	u32 *pmu_bl_gm10x = NULL;
	DEFINE_DMA_ATTRS(attrs);
	gm20b_dbg_pmu("");

	hsbl_fw = gk20a_request_firmware(g, GM20B_HSBIN_PMU_BL_UCODE_IMAGE);
	if (!hsbl_fw) {
		gk20a_err(dev_from_gk20a(g), "failed to load pmu ucode!!");
		return -ENOENT;
	}
	acr->bl_bin_hdr = (struct bin_hdr *)hsbl_fw->data;
	acr->pmu_hsbl_desc = (struct hsflcn_bl_desc *)(hsbl_fw->data +
		acr->bl_bin_hdr->header_offset);
	pmu_bl_gm10x_desc = acr->pmu_hsbl_desc;
	pmu_bl_gm10x = (u32 *)(hsbl_fw->data + acr->bl_bin_hdr->data_offset);
	bl_sz = ALIGN(pmu_bl_gm10x_desc->bl_img_hdr.bl_code_size,
			256);
	gm20b_dbg_pmu("Executing Generic Bootloader\n");

	/*TODO in code verify that enable PMU is done, scrubbing etc is done*/
	/*TODO in code verify that gmmu vm init is done*/
	/*
	 * Disable interrupts to avoid kernel hitting breakpoint due
	 * to PMU halt
	 */

	gk20a_writel(g, pwr_falcon_irqsclr_r(),
		gk20a_readl(g, pwr_falcon_irqsclr_r()) & (~(0x10)));

	dma_set_attr(DMA_ATTR_READ_ONLY, &attrs);
	bl_cpuva = dma_alloc_attrs(d, bl_sz,
			&iova,
			GFP_KERNEL,
			&attrs);
	gm20b_dbg_pmu("bl size is %x\n", bl_sz);
	if (!bl_cpuva) {
		gk20a_err(d, "failed to allocate memory\n");
		err = -ENOMEM;
		goto err_done;
	}

	err = gk20a_get_sgtable(d, &sgt_pmu_ucode,
			bl_cpuva,
			iova,
			bl_sz);
	if (err) {
		gk20a_err(d, "failed to allocate sg table\n");
		goto err_free_cpu_va;
	}

	bl_pmu_va = gk20a_gmmu_map(vm, &sgt_pmu_ucode,
			bl_sz,
			0, /* flags */
			gk20a_mem_flag_read_only);
	if (!bl_pmu_va) {
		gk20a_err(d, "failed to map pmu ucode memory!!");
		goto err_free_ucode_sgt;
	}

	for (i = 0; i < (bl_sz) >> 2; i++)
		gk20a_mem_wr32(bl_cpuva, i, pmu_bl_gm10x[i]);
	gm20b_dbg_pmu("Copied bl ucode to bl_cpuva\n");
	pmu->ucode.cpuva = bl_cpuva;
	pmu->ucode.pmu_va = bl_pmu_va;
	gm20b_init_pmu_setup_hw1(g, desc, bl_sz);
	/* Poll for HALT */
	if (b_wait_for_halt) {
		err = pmu_wait_for_halt(g, GPU_TIMEOUT_DEFAULT);
		if (err == 0)
			/* Clear the HALT interrupt */
			gk20a_writel(g, pwr_falcon_irqsclr_r(),
			gk20a_readl(g, pwr_falcon_irqsclr_r()) & (~(0x10)));
		else
			goto err_unmap_bl;
	}
	gm20b_dbg_pmu("after waiting for halt, err %x\n", err);
	gm20b_dbg_pmu("err reg :%x\n", readl(mc +
		MC_ERR_GENERALIZED_CARVEOUT_STATUS_0));
	gm20b_dbg_pmu("phys sec reg %x\n", gk20a_readl(g,
		pwr_falcon_mmu_phys_sec_r()));
	gm20b_dbg_pmu("sctl reg %x\n", gk20a_readl(g, pwr_falcon_sctl_r()));
	start_gm20b_pmu(g);
	err = 0;
err_unmap_bl:
	gk20a_gmmu_unmap(vm, pmu->ucode.pmu_va,
			bl_sz, gk20a_mem_flag_none);
err_free_ucode_sgt:
	gk20a_free_sgtable(&sgt_pmu_ucode);
err_free_cpu_va:
	dma_free_attrs(d, bl_sz,
			bl_cpuva, iova, &attrs);
err_done:
	return err;
}

/*!
*	Wait for PMU to halt
*	@param[in]	g		GPU object pointer
*	@param[in]	timeout_us	Timeout in Us for PMU to halt
*	@return '0' if PMU halts
*/
int pmu_wait_for_halt(struct gk20a *g, unsigned int timeout)
{
	u32 data = 0;
	udelay(10);
	data = gk20a_readl(g, pwr_falcon_cpuctl_r());
	gm20b_dbg_pmu("bef while cpuctl %xi, timeout %d\n", data, timeout);
	while (timeout != 0) {
		data = gk20a_readl(g, pwr_falcon_cpuctl_r());
		if (data & pwr_falcon_cpuctl_halt_intr_m())
			/*CPU is halted break*/
			break;
		timeout--;
		udelay(1);
	}
	if (timeout == 0)
		return -EBUSY;
	return 0;
}
