/*
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

#include <linux/fs.h>
#include <linux/file.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/nvhost.h>
#include <linux/nvhost_gpu_ioctl.h>
#include <linux/anon_inodes.h>

#include "gk20a.h"

bool gk20a_is_channel_marked_as_tsg(struct channel_gk20a *ch)
{
	return !(ch->tsgid == NVGPU_INVALID_TSG_ID);
}

/*
 * API to add channel to runnable list of TSG.
 *
 * After this call, a channel will be scheduled as TSG channel
 * in runlist
 */
int gk20a_bind_runnable_channel_to_tsg(struct channel_gk20a *ch, int tsgid)
{
	struct gk20a *g = ch->g;
	struct tsg_gk20a *tsg = NULL;

	if (ch->tsgid != tsgid)
		return -EINVAL;

	tsg = &g->fifo.tsg[tsgid];

	mutex_lock(&tsg->ch_list_lock);
	list_add_tail(&ch->ch_entry, &tsg->ch_runnable_list);
	tsg->num_runnable_channels += 1;
	mutex_unlock(&tsg->ch_list_lock);

	return tsg->num_runnable_channels;
}

int gk20a_unbind_channel_from_tsg(struct channel_gk20a *ch, int tsgid)
{
	struct gk20a *g = ch->g;
	struct tsg_gk20a *tsg = NULL;

	if (ch->tsgid != tsgid)
		return -EINVAL;

	tsg = &g->fifo.tsg[tsgid];

	mutex_lock(&tsg->ch_list_lock);
	list_del_init(&ch->ch_entry);
	tsg->num_runnable_channels -= 1;
	mutex_unlock(&tsg->ch_list_lock);

	return tsg->num_runnable_channels;
}

/*
 * API to mark channel as part of TSG
 *
 * Note that channel is not runnable when we bind it to TSG
 */
static int nvgpu_tsg_bind_channel(struct tsg_gk20a *tsg, int ch_fd)
{
	struct file *f = fget(ch_fd);
	struct channel_gk20a *ch = f->private_data;

	/* check if channel is already bound to some TSG */
	if (gk20a_is_channel_marked_as_tsg(ch))
		return -EINVAL;

	ch->tsgid = tsg->tsgid;

	gk20a_dbg(gpu_dbg_fn, "BIND tsg:%d channel:%d\n",
					tsg->tsgid, ch->hw_chid);

	fput(f);

	return 0;
}

static int nvgpu_tsg_unbind_channel(struct tsg_gk20a *tsg, int ch_fd)
{
	/* We do not support explicitly unbinding channel from TSG.
	 * Channel will be unbounded from TSG when it is closed.
	 */
	return 0;
}

int gk20a_init_tsg_support(struct gk20a *g, u32 tsgid)
{
	struct tsg_gk20a *tsg = NULL;

	if (tsgid < 0 || tsgid >= g->fifo.num_channels)
		return -EINVAL;

	tsg = &g->fifo.tsg[tsgid];

	tsg->in_use = false;
	tsg->tsgid = tsgid;

	INIT_LIST_HEAD(&tsg->ch_runnable_list);
	mutex_init(&tsg->ch_list_lock);

	return 0;
}

static void release_used_tsg(struct fifo_gk20a *f, struct tsg_gk20a *tsg)
{
	mutex_lock(&f->tsg_inuse_mutex);
	f->tsg[tsg->tsgid].in_use = false;
	mutex_unlock(&f->tsg_inuse_mutex);
}

static struct tsg_gk20a *acquire_unused_tsg(struct fifo_gk20a *f)
{
	struct tsg_gk20a *tsg = NULL;
	int tsgid;

	mutex_lock(&f->tsg_inuse_mutex);
	for (tsgid = 0; tsgid < f->num_channels; tsgid++) {
		if (!f->tsg[tsgid].in_use) {
			f->tsg[tsgid].in_use = true;
			tsg = &f->tsg[tsgid];
			break;
		}
	}
	mutex_unlock(&f->tsg_inuse_mutex);

	return tsg;
}

int gk20a_tsg_dev_open(struct inode *inode, struct file *filp)
{
	struct tsg_gk20a *tsg;
	struct gk20a *g;
	struct device *dev;

	g = container_of(inode->i_cdev,
			 struct gk20a, tsg.cdev);
	dev  = dev_from_gk20a(g);

	gk20a_dbg(gpu_dbg_fn, "tsg: %s", dev_name(dev));

	tsg = acquire_unused_tsg(&g->fifo);
	if (!tsg)
		return -ENOMEM;

	tsg->g = g;
	tsg->num_runnable_channels = 0;

	tsg->tsg_gr_ctx = NULL;
	tsg->vm = NULL;

	filp->private_data = tsg;

	gk20a_dbg(gpu_dbg_fn, "tsg opened %d\n", tsg->tsgid);

	return 0;
}

int gk20a_tsg_dev_release(struct inode *inode, struct file *filp)
{
	struct tsg_gk20a *tsg = filp->private_data;
	struct gk20a *g = container_of(inode->i_cdev,
			 struct gk20a, tsg.cdev);

	if (tsg->num_runnable_channels) {
		gk20a_err(dev_from_gk20a(g),
			"Trying to free TSG %d with active channels %d\n",
			tsg->tsgid, tsg->num_runnable_channels);
		return -EBUSY;
	}

	if (tsg->tsg_gr_ctx) {
		gr_gk20a_free_tsg_gr_ctx(tsg);
		tsg->tsg_gr_ctx = NULL;
	}
	if (tsg->vm)
		tsg->vm = NULL;

	release_used_tsg(&g->fifo, tsg);

	gk20a_dbg(gpu_dbg_fn, "tsg released %d\n", tsg->tsgid);

	return 0;
}

long gk20a_tsg_dev_ioctl(struct file *filp, unsigned int cmd,
			     unsigned long arg)
{
	struct tsg_gk20a *tsg = filp->private_data;
	struct gk20a *g = tsg->g;
	u8 __maybe_unused buf[NVGPU_TSG_IOCTL_MAX_ARG_SIZE];
	int err = 0;

	gk20a_dbg(gpu_dbg_fn, "");

	if ((_IOC_TYPE(cmd) != NVGPU_TSG_IOCTL_MAGIC) ||
	    (_IOC_NR(cmd) == 0) ||
	    (_IOC_NR(cmd) > NVGPU_TSG_IOCTL_LAST))
		return -EFAULT;

	BUG_ON(_IOC_SIZE(cmd) > NVGPU_TSG_IOCTL_MAX_ARG_SIZE);

	if (_IOC_DIR(cmd) & _IOC_WRITE) {
		if (copy_from_user(buf, (void __user *)arg, _IOC_SIZE(cmd)))
			return -EFAULT;
	}

	if (!g->gr.sw_ready) {
		err = gk20a_busy(g->dev);
		if (err)
			return err;

		gk20a_idle(g->dev);
	}

	switch (cmd) {
	case NVGPU_TSG_IOCTL_BIND_CHANNEL:
		{
		int ch_fd = *(int *)buf;
		if (ch_fd < 0) {
			err = -EINVAL;
			break;
		}
		err = nvgpu_tsg_bind_channel(tsg, ch_fd);
		break;
		}

	case NVGPU_TSG_IOCTL_UNBIND_CHANNEL:
		{
		int ch_fd = *(int *)buf;
		if (ch_fd < 0) {
			err = -EINVAL;
			break;
		}
		err = nvgpu_tsg_unbind_channel(tsg, ch_fd);
		break;
		}

	default:
		gk20a_err(dev_from_gk20a(g),
			   "unrecognized tsg gpu ioctl cmd: 0x%x",
			   cmd);
		err = -ENOTTY;
		break;
	}

	if ((err == 0) && (_IOC_DIR(cmd) & _IOC_READ))
		err = copy_to_user((void __user *)arg,
				   buf, _IOC_SIZE(cmd));

	return err;
}
