/*
 * drivers/video/tegra/host/gk20a/channel_sync_gk20a.h
 *
 * GK20A Channel Synchronization Abstraction
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

#ifndef _GK20A_CHANNEL_SYNC_H_
#define _GK20A_CHANNEL_SYNC_H_

#include <linux/types.h>

struct gk20a_channel_sync;
struct priv_cmd_entry;
struct channel_gk20a;
struct gk20a_semaphore;
struct gk20a_fence;

struct gk20a_channel_sync {
	/* Generate a gpu wait cmdbuf from syncpoint.
	 * Returns
	 *  - a gpu cmdbuf that performs the wait when executed,
	 *  - possibly a helper fence that the caller must hold until the
	 *    cmdbuf is executed.
	 */
	int (*wait_syncpt)(struct gk20a_channel_sync *s, u32 id, u32 thresh,
			   struct priv_cmd_entry **entry,
			   struct gk20a_fence **fence);

	/* Generate a gpu wait cmdbuf from sync fd.
	 * Returns
	 *  - a gpu cmdbuf that performs the wait when executed,
	 *  - possibly a helper fence that the caller must hold until the
	 *    cmdbuf is executed.
	 */
	int (*wait_fd)(struct gk20a_channel_sync *s, int fd,
		       struct priv_cmd_entry **entry,
		       struct gk20a_fence **fence);

	/* Increment syncpoint/semaphore.
	 * Returns
	 *  - a gpu cmdbuf that performs the increment when executed,
	 *  - a fence that can be passed to wait_cpu() and is_expired().
	 */
	int (*incr)(struct gk20a_channel_sync *s,
		    struct priv_cmd_entry **entry,
		    struct gk20a_fence **fence);

	/* Increment syncpoint/semaphore, preceded by a wfi.
	 * Returns
	 *  - a gpu cmdbuf that performs the increment when executed,
	 *  - a fence that can be passed to wait_cpu() and is_expired().
	 */
	int (*incr_wfi)(struct gk20a_channel_sync *s,
			struct priv_cmd_entry **entry,
			struct gk20a_fence **fence);

	/* Increment syncpoint/semaphore, so that the returned fence represents
	 * work completion (may need wfi) and can be returned to user space.
	 * Returns
	 *  - a gpu cmdbuf that performs the increment when executed,
	 *  - a fence that can be passed to wait_cpu() and is_expired(),
	 *  - a gk20a_fence that signals when the incr has happened.
	 */
	int (*incr_user)(struct gk20a_channel_sync *s,
			 int wait_fence_fd,
			 struct priv_cmd_entry **entry,
			 struct gk20a_fence **fence,
			 bool wfi);

	/* Reset the channel syncpoint/semaphore. */
	void (*set_min_eq_max)(struct gk20a_channel_sync *s);

	/* Signals the sync timeline (if owned by the gk20a_channel_sync layer).
	 * This should be called when we notice that a gk20a_fence is
	 * expired. */
	void (*signal_timeline)(struct gk20a_channel_sync *s);

	/* flag to set sync destroy aggressiveness */
	bool aggressive_destroy;

	/* Free the resources allocated by gk20a_channel_sync_create. */
	void (*destroy)(struct gk20a_channel_sync *s);
};

struct gk20a_channel_sync *gk20a_channel_sync_create(struct channel_gk20a *c);

#endif
