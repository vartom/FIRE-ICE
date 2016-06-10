/*
 * Tegra Graphics Virtualization Host Interrupt Management
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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/delay.h>

#include "nvhost_intr.h"
#include "vhost.h"
#include "dev.h"

/* Spacing between sync registers */
#define REGISTER_STRIDE 4

static void syncpt_thresh_cascade_fn(struct work_struct *work)
{
	struct nvhost_intr_syncpt *sp =
		container_of(work, struct nvhost_intr_syncpt, work);
	nvhost_syncpt_thresh_fn(sp);
}

static int syncpt_thresh_cascade_handler(void *dev_id)
{
	while (true) {
		struct nvhost_master *dev = dev_id;
		struct nvhost_intr *intr = &dev->intr;
		struct nvhost_intr_syncpt *sp;
		struct tegra_vhost_syncpt_intr_msg *msg;
		u32 sp_id, sender;
		void *handle;
		size_t size;
		int err;
		int graphics_host_sp =
				nvhost_syncpt_graphics_host_sp(&dev->syncpt);

		err = tegra_gr_comm_recv(TEGRA_GR_COMM_CTX_CLIENT,
					TEGRA_VHOST_QUEUE_INTR, &handle,
					(void **)&msg, &size, &sender);
		if (WARN_ON(err))
			continue;

		if (msg->event == TEGRA_VHOST_EVENT_ABORT) {
			tegra_gr_comm_release(handle);
			break;
		}

		sp_id = msg->id;
		if (unlikely(sp_id < dev->info.pts_base ||
			sp_id >= dev->info.pts_limit)) {
			dev_err(&dev->dev->dev,
				"%s(): syncpoint id %d is beyond the number of syncpoints (%d)\n",
				__func__, sp_id, dev->info.nb_pts);
			tegra_gr_comm_release(handle);
			continue;
		}

		sp = intr->syncpt + sp_id;
		ktime_get_ts(&sp->isr_recv);
		nvhost_syncpt_set_min_cached(&dev->syncpt, sp_id, msg->thresh);

		/* handle graphics host syncpoint increments immediately */
		if (sp_id == graphics_host_sp) {
			dev_warn(&dev->dev->dev, "%s(): syncpoint id %d incremented\n",
				 __func__, graphics_host_sp);
			nvhost_syncpt_patch_check(&dev->syncpt);
		} else
			queue_work(intr->wq, &sp->work);

		tegra_gr_comm_release(handle);
	}

	while (!kthread_should_stop())
		msleep(10);
	return 0;
}

static void vhost_syncpt_enable_intr(u64 handle, u32 id, u32 thresh)
{
	struct tegra_vhost_cmd_msg msg;
	struct tegra_vhost_syncpt_intr_params *p = &msg.params.syncpt_intr;
	int err;

	msg.cmd = TEGRA_VHOST_CMD_SYNCPT_ENABLE_INTR;
	msg.handle = handle;
	p->id = id;
	p->thresh = thresh;
	err = vhost_sendrecv(&msg);
	WARN_ON(err || msg.ret);
}

static void vhost_intr_init_host_sync(struct nvhost_intr *intr)
{
	struct nvhost_master *dev = intr_to_dev(intr);
	struct nvhost_virt_ctx *ctx = nvhost_get_virt_data(dev->dev);
	int i;

	intr_op().disable_all_syncpt_intrs(intr);

	for (i = 0; i < dev->info.nb_pts; i++)
		INIT_WORK(&intr->syncpt[i].work, syncpt_thresh_cascade_fn);

	ctx->syncpt_handler =
		kthread_run(syncpt_thresh_cascade_handler, dev, "host_syncpt");
	if (IS_ERR(ctx->syncpt_handler))
		BUG();

	vhost_syncpt_enable_intr(ctx->handle,
			nvhost_syncpt_graphics_host_sp(&dev->syncpt), 1);
}

static void vhost_intr_set_host_clocks_per_usec(struct nvhost_intr *intr,
						u32 cpm)
{
}

static void vhost_intr_set_syncpt_threshold(struct nvhost_intr *intr,
					u32 id, u32 thresh)
{
	struct nvhost_master *dev = intr_to_dev(intr);
	struct nvhost_virt_ctx *ctx = nvhost_get_virt_data(dev->dev);

	vhost_syncpt_enable_intr(ctx->handle, id, thresh);
}

static void vhost_intr_enable_syncpt_intr(struct nvhost_intr *intr, u32 id)
{
	/* syncpt is enabled when threshold is set */
}

static void vhost_intr_disable_syncpt_intr(struct nvhost_intr *intr, u32 id)
{
	struct nvhost_master *dev = intr_to_dev(intr);
	struct nvhost_virt_ctx *ctx = nvhost_get_virt_data(dev->dev);
	struct tegra_vhost_cmd_msg msg;
	struct tegra_vhost_syncpt_intr_params *p = &msg.params.syncpt_intr;
	int err;

	msg.cmd = TEGRA_VHOST_CMD_SYNCPT_DISABLE_INTR;
	msg.handle = ctx->handle;
	p->id = id;
	err = vhost_sendrecv(&msg);
	WARN_ON(err || msg.ret);
}

static void vhost_intr_disable_all_syncpt_intrs(struct nvhost_intr *intr)
{
	struct nvhost_master *dev = intr_to_dev(intr);
	struct nvhost_virt_ctx *ctx = nvhost_get_virt_data(dev->dev);
	struct tegra_vhost_cmd_msg msg;
	int err;

	msg.cmd = TEGRA_VHOST_CMD_SYNCPT_DISABLE_INTR_ALL;
	msg.handle = ctx->handle;
	err = vhost_sendrecv(&msg);
	WARN_ON(err || msg.ret);
}

static int vhost_intr_request_host_general_irq(struct nvhost_intr *intr)
{
	return 0;
}

static void vhost_intr_free_host_general_irq(struct nvhost_intr *intr)
{
}

static int vhost_free_syncpt_irq(struct nvhost_intr *intr)
{
	struct nvhost_master *dev = intr_to_dev(intr);
	struct nvhost_virt_ctx *ctx = nvhost_get_virt_data(dev->dev);
	struct tegra_vhost_syncpt_intr_msg msg;
	int err;

	/* disable graphics host syncpoint interrupt */
	vhost_intr_disable_syncpt_intr(intr,
			nvhost_syncpt_graphics_host_sp(&dev->syncpt));

	msg.event = TEGRA_VHOST_EVENT_ABORT;
	err = tegra_gr_comm_send(TEGRA_GR_COMM_CTX_CLIENT,
				TEGRA_GR_COMM_ID_SELF, TEGRA_VHOST_QUEUE_INTR,
				&msg, sizeof(msg));
	WARN_ON(err);
	kthread_stop(ctx->syncpt_handler);
	flush_workqueue(intr->wq);
	return 0;
}

void vhost_init_host1x_intr_ops(struct nvhost_intr_ops *ops)
{
	ops->init_host_sync = vhost_intr_init_host_sync;
	ops->set_host_clocks_per_usec = vhost_intr_set_host_clocks_per_usec;
	ops->set_syncpt_threshold = vhost_intr_set_syncpt_threshold;
	ops->enable_syncpt_intr = vhost_intr_enable_syncpt_intr;
	ops->disable_syncpt_intr = vhost_intr_disable_syncpt_intr;
	ops->disable_all_syncpt_intrs = vhost_intr_disable_all_syncpt_intrs;
	ops->request_host_general_irq = vhost_intr_request_host_general_irq;
	ops->free_host_general_irq = vhost_intr_free_host_general_irq;
	ops->free_syncpt_irq = vhost_free_syncpt_irq;
}
