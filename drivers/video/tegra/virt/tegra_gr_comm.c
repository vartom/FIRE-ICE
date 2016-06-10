/*
 * Tegra Graphics Virtualization Communication Framework
 *
 * Copyright (c) 2013-2014, NVIDIA Corporation. All rights reserved.
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

#include <linux/mutex.h>
#include <linux/semaphore.h>
#include <linux/list.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/idr.h>
#include <linux/tegra-ivc.h>
#include <linux/tegra_gr_comm.h>

#define NUM_QUEUES   4
#define NUM_CONTEXTS 2

#define ID_SHIFT 11
#define ID_MASK 1
#define ID0_PEER_ID_SHIFT 4
#define ID0_QUEUE_ID_SHIFT 1
#define ID0_QUEUE_ID_MASK 7
#define ID0_VCTX_SHIFT 0
#define ID0_VCTX_MASK 1
#define ID1_IRQ_SHIFT 0

#define GEN_ID0(vctx, queue_id, peer_id)	\
	((peer_id << ID0_PEER_ID_SHIFT) |	\
	(queue_id << ID0_QUEUE_ID_SHIFT) |	\
	(vctx << ID0_VCTX_SHIFT) |		\
	(0 << ID_SHIFT))
#define GEN_ID1(irq)				\
	((irq << ID1_IRQ_SHIFT) |		\
	(1 << ID_SHIFT))
#define IS_ID1(id) ((id >> ID_SHIFT) & ID_MASK)
#define VCTX_FROM_ID0(id) ((id >> ID0_VCTX_SHIFT) & ID0_VCTX_MASK)
#define QUEUE_ID_FROM_ID0(id) ((id >> ID0_QUEUE_ID_SHIFT) & ID0_QUEUE_ID_MASK)

enum {
	IVC_DIR_TX = 0,
	IVC_DIR_RX,
	NUM_IVC_DIR
};

struct gr_comm_ivc_context {
	u32 peer;
	wait_queue_head_t wq[NUM_IVC_DIR];
	struct tegra_hv_ivc_cookie *cookie;
	struct gr_comm_queue *queue;
	struct task_struct *rx_thread;
	struct platform_device *pdev;
};

struct gr_comm_element {
	u32 sender;
	size_t size;
	void *data;
	struct list_head list;
	struct gr_comm_queue *queue;
};

struct gr_comm_queue {
	struct semaphore sem;
	struct mutex lock;
	struct mutex resp_lock;
	struct list_head pending;
	struct list_head free;
	size_t size;
	struct kmem_cache *element_cache;
	bool valid;
};

struct gr_comm_context {
	struct gr_comm_queue queue[NUM_QUEUES];
};

enum {
	PROP_IVC_NODE = 0,
	PROP_IVC_INST,
	NUM_PROP
};

static struct gr_comm_context contexts[NUM_CONTEXTS];
static DEFINE_IDR(ivc_ctx_idr);
static u32 server_vmid;

u32 tegra_gr_comm_get_server_vmid(void)
{
	return server_vmid;
}

static void free_ivc(u32 virt_ctx, u32 queue_start, u32 queue_end)
{
	struct gr_comm_ivc_context *tmp;
	int id;

	idr_for_each_entry(&ivc_ctx_idr, tmp, id) {
		if (VCTX_FROM_ID0(id) == virt_ctx &&
			QUEUE_ID_FROM_ID0(id) >= queue_start &&
			QUEUE_ID_FROM_ID0(id) < queue_end) {
			idr_remove(&ivc_ctx_idr, id);
			if (tmp->rx_thread)
				kthread_stop(tmp->rx_thread);

			if (tmp->cookie) {
				idr_remove(&ivc_ctx_idr,
					GEN_ID1(tmp->cookie->irq));
				tegra_hv_ivc_unreserve(tmp->cookie);
			}
			kfree(tmp);
		}
	}
}

static void ivc_rx(struct tegra_hv_ivc_cookie *ivck)
{
	struct gr_comm_ivc_context *ctx =
			idr_find(&ivc_ctx_idr, GEN_ID1(ivck->irq));

	if (!ctx)
		return;

	wake_up(&ctx->wq[IVC_DIR_RX]);
}

static void ivc_tx(struct tegra_hv_ivc_cookie *ivck)
{
	struct gr_comm_ivc_context *ctx =
			idr_find(&ivc_ctx_idr, GEN_ID1(ivck->irq));

	if (!ctx)
		return;

	wake_up(&ctx->wq[IVC_DIR_TX]);
}

static const struct tegra_hv_ivc_ops ivc_ops = { ivc_rx, ivc_tx };

static int queue_add(struct gr_comm_queue *queue, const char *data,
		u32 peer, struct tegra_hv_ivc_cookie *ivck)
{
	struct gr_comm_element *element;

	mutex_lock(&queue->lock);
	if (list_empty(&queue->free)) {
		element = kmem_cache_alloc(queue->element_cache,
					GFP_KERNEL);
		if (!element) {
			mutex_unlock(&queue->lock);
			return -ENOMEM;
		}
		element->data = (char *)element + sizeof(*element);
		element->queue = queue;
	} else {
		element = list_first_entry(&queue->free,
				struct gr_comm_element, list);
		list_del(&element->list);
	}

	element->sender = peer;
	element->size = queue->size;
	if (ivck) {
		int ret = tegra_hv_ivc_read(ivck, element->data, element->size);
		if (ret != element->size) {
			list_add(&element->list, &queue->free);
			mutex_unlock(&queue->lock);
			return -ENOMEM;
		}
	} else
		memcpy(element->data, data, element->size);
	list_add_tail(&element->list, &queue->pending);
	mutex_unlock(&queue->lock);
	up(&queue->sem);
	return 0;
}

static int ivc_rx_thread(void *arg)
{
	struct gr_comm_ivc_context *ctx = (struct gr_comm_ivc_context *)arg;
	struct device *dev = &ctx->pdev->dev;
	int ret;

	while (!kthread_should_stop()) {
		if (!tegra_hv_ivc_can_read(ctx->cookie)) {
				ret = wait_event_timeout(ctx->wq[IVC_DIR_RX],
					tegra_hv_ivc_can_read(ctx->cookie) ||
					kthread_should_stop(),
					msecs_to_jiffies(250));

				if (kthread_should_stop())
					break;
				else if (!ret)
					continue;
		}

		if (queue_add(ctx->queue, NULL, ctx->peer, ctx->cookie)) {
			dev_err(dev, "%s cannot add to queue\n", __func__);
			continue;
		}
	}

	return 0;
}

static int setup_ivc(u32 virt_ctx, struct platform_device *pdev,
		u32 queue_start, u32 queue_end)
{
	struct device *dev = &pdev->dev;
	int i, j, ret = -EINVAL;

	for (i = queue_start; i < queue_end; ++i) {
		char name[20];
		u32 inst;

		snprintf(name, sizeof(name), "ivc-queue%d", i);
		for (j = 0;
			of_property_read_u32_index(dev->of_node, name,
				j * NUM_PROP + PROP_IVC_INST, &inst) == 0;
				j++) {
			struct device_node *hv_dn;
			struct gr_comm_ivc_context *ctx;
			struct gr_comm_queue *queue =
					&contexts[virt_ctx].queue[i];
			char thread_name[30];
			int id, err;

			hv_dn = of_parse_phandle(dev->of_node, name,
						j * NUM_PROP + PROP_IVC_NODE);
			if (!hv_dn)
				goto fail;

			ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
			if (!ctx) {
				ret = -ENOMEM;
				goto fail;
			}

			ctx->pdev = pdev;
			ctx->queue = queue;
			init_waitqueue_head(&ctx->wq[IVC_DIR_TX]);
			init_waitqueue_head(&ctx->wq[IVC_DIR_RX]);

			ctx->cookie =
				tegra_hv_ivc_reserve(hv_dn, inst, &ivc_ops);
			if (IS_ERR_OR_NULL(ctx->cookie)) {
				ret = PTR_ERR(ctx->cookie);
				kfree(ctx);
				goto fail;
			}

			if (ctx->cookie->frame_size < queue->size) {
				ret = -ENOMEM;
				tegra_hv_ivc_unreserve(ctx->cookie);
				kfree(ctx);
				goto fail;
			}

			ctx->peer = ctx->cookie->peer_vmid;

			id = GEN_ID0(virt_ctx, i, ctx->peer);
			err = idr_alloc(&ivc_ctx_idr, ctx, id, id + 1,
					GFP_KERNEL);
			if (err != id) {
				tegra_hv_ivc_unreserve(ctx->cookie);
				kfree(ctx);
				goto fail;
			}

			id = GEN_ID1(ctx->cookie->irq);
			err = idr_alloc(&ivc_ctx_idr, ctx, id, id + 1,
					GFP_KERNEL);
			if (err != id)
				goto fail;

			server_vmid = ctx->peer;
			snprintf(thread_name, sizeof(thread_name),
				"gr-virt-thr-%d-%d-%d", virt_ctx, i, ctx->peer);
			ctx->rx_thread =
				kthread_run(ivc_rx_thread, ctx, thread_name);
			if (IS_ERR(ctx->rx_thread))
				goto fail;
		}
		if (j == 0)
			goto fail;
	}

	return 0;

fail:
	free_ivc(virt_ctx, queue_start, queue_end);
	return ret;
}

int tegra_gr_comm_init(struct platform_device *pdev, u32 virt_ctx, u32 elems,
		const size_t *queue_sizes, u32 queue_start, u32 num_queues)
{
	struct gr_comm_context *ctx;
	int i = 0, j;
	int ret = 0;
	struct device *dev = &pdev->dev;
	u32 queue_end = queue_start + num_queues;

	if (virt_ctx >= NUM_CONTEXTS || queue_end > NUM_QUEUES)
		return -EINVAL;

	ctx = &contexts[virt_ctx];
	for (i = queue_start; i < queue_end; ++i) {
		char name[30];
		size_t size = queue_sizes[i - queue_start];
		struct gr_comm_queue *queue = &ctx->queue[i];

		if (queue->valid)
			return -EEXIST;

		snprintf(name, sizeof(name), "gr-virt-comm-%d-%d", virt_ctx, i);
		queue->element_cache =
			kmem_cache_create(name,
				sizeof(struct gr_comm_element) + size, 0,
				SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD, NULL);

		if (!queue->element_cache) {
			ret = -ENOMEM;
			goto fail;
		}

		sema_init(&queue->sem, 0);
		mutex_init(&queue->lock);
		mutex_init(&queue->resp_lock);
		INIT_LIST_HEAD(&queue->free);
		INIT_LIST_HEAD(&queue->pending);
		queue->size = size;

		for (j = 0; j < elems; ++j) {
			struct gr_comm_element *element =
				kmem_cache_alloc(queue->element_cache,
						GFP_KERNEL);

			if (!element) {
				ret = -ENOMEM;
				goto fail;
			}
			element->data = (char *)element + sizeof(*element);
			element->queue = queue;
			list_add(&element->list, &queue->free);
		}

		queue->valid = true;
	}

	ret = setup_ivc(virt_ctx, pdev, queue_start, queue_end);
	if (ret) {
		dev_err(dev, "invalid IVC DT data\n");
		goto fail;
	}

	return 0;

fail:
	while (i >= 0) {
		struct gr_comm_element *tmp, *next;
		struct gr_comm_queue *queue = &ctx->queue[i];

		if (queue->element_cache) {
			list_for_each_entry_safe(tmp, next, &queue->free,
					list) {
				list_del(&tmp->list);
				kmem_cache_free(queue->element_cache, tmp);
			}
		}
		kmem_cache_destroy(queue->element_cache);
		--i;
	}

	free_ivc(virt_ctx, queue_start, queue_end);
	dev_err(dev, "%s insufficient memory\n", __func__);
	return ret;
}

void tegra_gr_comm_deinit(u32 virt_ctx, u32 queue_start, u32 num_queues)
{
	struct gr_comm_context *ctx;
	struct gr_comm_element *tmp, *next;
	u32 queue_end = queue_start + num_queues;
	int i;

	if (virt_ctx >= NUM_CONTEXTS || queue_end > NUM_QUEUES)
		return;

	ctx = &contexts[virt_ctx];

	for (i = queue_start; i < queue_end; ++i) {
		struct gr_comm_queue *queue = &ctx->queue[i];

		if (!queue->valid)
			continue;

		list_for_each_entry_safe(tmp, next, &queue->free, list) {
			list_del(&tmp->list);
			kmem_cache_free(queue->element_cache, tmp);
		}

		list_for_each_entry_safe(tmp, next, &queue->pending, list) {
			list_del(&tmp->list);
			kmem_cache_free(queue->element_cache, tmp);
		}
		kmem_cache_destroy(queue->element_cache);
		queue->valid = false;
	}
	free_ivc(virt_ctx, queue_start, queue_end);
}

int tegra_gr_comm_send(u32 virt_ctx, u32 peer, u32 index, void *data,
		size_t size)
{
	struct gr_comm_context *ctx;
	struct gr_comm_ivc_context *ivc_ctx;
	int ret;

	if (virt_ctx >= NUM_CONTEXTS || index >= NUM_QUEUES)
		return -EINVAL;

	ctx = &contexts[virt_ctx];
	if (!ctx->queue[index].valid)
		return -EINVAL;

	if (peer == TEGRA_GR_COMM_ID_SELF)
		return queue_add(&ctx->queue[index], data, peer, NULL);

	ivc_ctx = idr_find(&ivc_ctx_idr, GEN_ID0(virt_ctx, index, peer));
	if (!ivc_ctx)
		return -EINVAL;

	if (!tegra_hv_ivc_can_write(ivc_ctx->cookie)) {
		ret = wait_event_timeout(ivc_ctx->wq[IVC_DIR_TX],
				tegra_hv_ivc_can_write(ivc_ctx->cookie),
				msecs_to_jiffies(250));
		if (!ret) {
			dev_err(&ivc_ctx->pdev->dev,
				"%s timeout waiting for buffer\n", __func__);
			return -ENOMEM;
		}
	}

	ret = tegra_hv_ivc_write(ivc_ctx->cookie, data, size);
	return (ret != size) ? -ENOMEM : 0;
}

int tegra_gr_comm_recv(u32 virt_ctx, u32 index, void **handle, void **data,
		size_t *size, u32 *sender)
{
	struct gr_comm_context *ctx;
	struct gr_comm_queue *queue;
	struct gr_comm_element *element;

	if (virt_ctx >= NUM_CONTEXTS || index >= NUM_QUEUES)
		return -EINVAL;

	ctx = &contexts[virt_ctx];
	if (!ctx->queue[index].valid)
		return -EINVAL;

	queue = &ctx->queue[index];
	down(&queue->sem);
	mutex_lock(&queue->lock);
	element = list_first_entry(&queue->pending,
			struct gr_comm_element, list);
	list_del(&element->list);
	mutex_unlock(&queue->lock);
	*handle = element;
	*data = element->data;
	*size = element->size;
	if (sender)
		*sender = element->sender;
	return 0;
}

int tegra_gr_comm_sendrecv(u32 virt_ctx, u32 peer, u32 index, void **handle,
			void **data, size_t *size)
{
	struct gr_comm_context *ctx;
	struct gr_comm_queue *queue;
	int err = 0;

	if (virt_ctx >= NUM_CONTEXTS || index >= NUM_QUEUES)
		return -EINVAL;

	ctx = &contexts[virt_ctx];
	if (!ctx->queue[index].valid)
		return -EINVAL;

	queue = &ctx->queue[index];
	mutex_lock(&queue->resp_lock);
	err = tegra_gr_comm_send(virt_ctx, peer, index, *data, *size);
	if (err)
		goto fail;
	err = tegra_gr_comm_recv(virt_ctx, index, handle, data, size, NULL);
fail:
	mutex_unlock(&queue->resp_lock);
	return err;
}


void tegra_gr_comm_release(void *handle)
{
	struct gr_comm_element *element =
		(struct gr_comm_element *)handle;

	mutex_lock(&element->queue->lock);
	list_add(&element->list, &element->queue->free);
	mutex_unlock(&element->queue->lock);
}
