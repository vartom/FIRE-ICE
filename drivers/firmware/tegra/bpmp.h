/*
 * Copyright (c) 2013-2015, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _DRIVERS_BPMP_H
#define _DRIVERS_BPMP_H

#include <linux/kernel.h>
#include <linux/platform_device.h>

#define NR_CHANNELS		12
#define MSG_SZ			32
#define MSG_DATA_SZ		24

#define NR_THREAD_CH		4

#define NR_MRQS			32
#define __MRQ_ATTRS		0xff000000
#define __MRQ_INDEX(id)		((id) & ~__MRQ_ATTRS)

struct fops_entry {
	char *name;
	const struct file_operations *fops;
	mode_t mode;
};

struct bpmp_cpuidle_state {
	int id;
	const char *name;
};

struct mb_data {
	int code;
	int flags;
	u8 data[MSG_DATA_SZ];
};

extern struct mb_data *channel_area[NR_CHANNELS];

#ifdef CONFIG_DEBUG_FS
extern struct bpmp_cpuidle_state plat_cpuidle_state[];
#endif

extern struct device *device;
extern struct mutex bpmp_lock;
extern int connected;

int bpmp_clk_init(struct platform_device *pdev);
int bpmp_platdbg_init(struct dentry *root, struct platform_device *pdev);
int bpmp_mail_init(struct platform_device *pdev);
int bpmp_get_fwtag(void);
int bpmp_init_modules(struct platform_device *pdev);
void bpmp_cleanup_modules(void);
int bpmp_create_attrs(const struct fops_entry *fent, struct dentry *parent,
		void *data);

int bpmp_attach(void);
void bpmp_detach(void);

/* should be called from non-preemptible context */
int bpmp_post(int mrq, void *data, int sz);

/* should be called from non-preemptible context */
int bpmp_rpc(int mrq, void *ob_data, int ob_sz, void *ib_data, int ib_sz);

/* should be called from sleepable context */
int bpmp_threaded_rpc(int mrq, void *ob_data, int ob_sz,
		void *ib_data, int ib_sz);

int __bpmp_rpc(int mrq, void *ob_data, int ob_sz, void *ib_data, int ib_sz);
int bpmp_ping(void);
int bpmp_module_load(struct device *dev, const void *base, u32 size,
		u32 *handle);
int bpmp_module_unload(struct device *dev, u32 handle);
int bpmp_cpuidle_usage(int state);
uint64_t bpmp_cpuidle_time(int state);
int bpmp_write_trace(uint32_t phys, int size, int *eof);
int bpmp_modify_trace_mask(uint32_t clr, uint32_t set);
int bpmp_init_cpus_present(int nr_cpus);
int bpmp_query_tag(uint32_t phys);

#endif
