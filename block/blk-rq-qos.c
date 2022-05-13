// SPDX-License-Identifier: GPL-2.0

#include "blk-rq-qos.h"

static DEFINE_IDA(rq_qos_ida);
static int nr_rqos_blkcg_pols;
static DEFINE_MUTEX(rq_qos_mutex);
static LIST_HEAD(rq_qos_list);

/*
 * Increment 'v', if 'v' is below 'below'. Returns true if we succeeded,
 * false if 'v' + 1 would be bigger than 'below'.
 */
static bool atomic_inc_below(atomic_t *v, unsigned int below)
{
	unsigned int cur = atomic_read(v);

	for (;;) {
		unsigned int old;

		if (cur >= below)
			return false;
		old = atomic_cmpxchg(v, cur, cur + 1);
		if (old == cur)
			break;
		cur = old;
	}

	return true;
}

bool rq_wait_inc_below(struct rq_wait *rq_wait, unsigned int limit)
{
	return atomic_inc_below(&rq_wait->inflight, limit);
}

void __rq_qos_cleanup(struct rq_qos *rqos, struct bio *bio)
{
	do {
		if (rqos->ops->cleanup)
			rqos->ops->cleanup(rqos, bio);
		rqos = rqos->next;
	} while (rqos);
}

void __rq_qos_done(struct rq_qos *rqos, struct request *rq)
{
	do {
		if (rqos->ops->done)
			rqos->ops->done(rqos, rq);
		rqos = rqos->next;
	} while (rqos);
}

void __rq_qos_issue(struct rq_qos *rqos, struct request *rq)
{
	do {
		if (rqos->ops->issue)
			rqos->ops->issue(rqos, rq);
		rqos = rqos->next;
	} while (rqos);
}

void __rq_qos_requeue(struct rq_qos *rqos, struct request *rq)
{
	do {
		if (rqos->ops->requeue)
			rqos->ops->requeue(rqos, rq);
		rqos = rqos->next;
	} while (rqos);
}

void __rq_qos_throttle(struct rq_qos *rqos, struct bio *bio)
{
	do {
		if (rqos->ops->throttle)
			rqos->ops->throttle(rqos, bio);
		rqos = rqos->next;
	} while (rqos);
}

void __rq_qos_track(struct rq_qos *rqos, struct request *rq, struct bio *bio)
{
	do {
		if (rqos->ops->track)
			rqos->ops->track(rqos, rq, bio);
		rqos = rqos->next;
	} while (rqos);
}

void __rq_qos_merge(struct rq_qos *rqos, struct request *rq, struct bio *bio)
{
	do {
		if (rqos->ops->merge)
			rqos->ops->merge(rqos, rq, bio);
		rqos = rqos->next;
	} while (rqos);
}

void __rq_qos_done_bio(struct rq_qos *rqos, struct bio *bio)
{
	do {
		if (rqos->ops->done_bio)
			rqos->ops->done_bio(rqos, bio);
		rqos = rqos->next;
	} while (rqos);
}

void __rq_qos_queue_depth_changed(struct rq_qos *rqos)
{
	do {
		if (rqos->ops->queue_depth_changed)
			rqos->ops->queue_depth_changed(rqos);
		rqos = rqos->next;
	} while (rqos);
}

/*
 * Return true, if we can't increase the depth further by scaling
 */
bool rq_depth_calc_max_depth(struct rq_depth *rqd)
{
	unsigned int depth;
	bool ret = false;

	/*
	 * For QD=1 devices, this is a special case. It's important for those
	 * to have one request ready when one completes, so force a depth of
	 * 2 for those devices. On the backend, it'll be a depth of 1 anyway,
	 * since the device can't have more than that in flight. If we're
	 * scaling down, then keep a setting of 1/1/1.
	 */
	if (rqd->queue_depth == 1) {
		if (rqd->scale_step > 0)
			rqd->max_depth = 1;
		else {
			rqd->max_depth = 2;
			ret = true;
		}
	} else {
		/*
		 * scale_step == 0 is our default state. If we have suffered
		 * latency spikes, step will be > 0, and we shrink the
		 * allowed write depths. If step is < 0, we're only doing
		 * writes, and we allow a temporarily higher depth to
		 * increase performance.
		 */
		depth = min_t(unsigned int, rqd->default_depth,
			      rqd->queue_depth);
		if (rqd->scale_step > 0)
			depth = 1 + ((depth - 1) >> min(31, rqd->scale_step));
		else if (rqd->scale_step < 0) {
			unsigned int maxd = 3 * rqd->queue_depth / 4;

			depth = 1 + ((depth - 1) << -rqd->scale_step);
			if (depth > maxd) {
				depth = maxd;
				ret = true;
			}
		}

		rqd->max_depth = depth;
	}

	return ret;
}

/* Returns true on success and false if scaling up wasn't possible */
bool rq_depth_scale_up(struct rq_depth *rqd)
{
	/*
	 * Hit max in previous round, stop here
	 */
	if (rqd->scaled_max)
		return false;

	rqd->scale_step--;

	rqd->scaled_max = rq_depth_calc_max_depth(rqd);
	return true;
}

/*
 * Scale rwb down. If 'hard_throttle' is set, do it quicker, since we
 * had a latency violation. Returns true on success and returns false if
 * scaling down wasn't possible.
 */
bool rq_depth_scale_down(struct rq_depth *rqd, bool hard_throttle)
{
	/*
	 * Stop scaling down when we've hit the limit. This also prevents
	 * ->scale_step from going to crazy values, if the device can't
	 * keep up.
	 */
	if (rqd->max_depth == 1)
		return false;

	if (rqd->scale_step < 0 && hard_throttle)
		rqd->scale_step = 0;
	else
		rqd->scale_step++;

	rqd->scaled_max = false;
	rq_depth_calc_max_depth(rqd);
	return true;
}

struct rq_qos_wait_data {
	struct wait_queue_entry wq;
	struct task_struct *task;
	struct rq_wait *rqw;
	acquire_inflight_cb_t *cb;
	void *private_data;
	bool got_token;
};

static int rq_qos_wake_function(struct wait_queue_entry *curr,
				unsigned int mode, int wake_flags, void *key)
{
	struct rq_qos_wait_data *data = container_of(curr,
						     struct rq_qos_wait_data,
						     wq);

	/*
	 * If we fail to get a budget, return -1 to interrupt the wake up loop
	 * in __wake_up_common.
	 */
	if (!data->cb(data->rqw, data->private_data))
		return -1;

	data->got_token = true;
	smp_wmb();
	list_del_init(&curr->entry);
	wake_up_process(data->task);
	return 1;
}

/**
 * rq_qos_wait - throttle on a rqw if we need to
 * @rqw: rqw to throttle on
 * @private_data: caller provided specific data
 * @acquire_inflight_cb: inc the rqw->inflight counter if we can
 * @cleanup_cb: the callback to cleanup in case we race with a waker
 *
 * This provides a uniform place for the rq_qos users to do their throttling.
 * Since you can end up with a lot of things sleeping at once, this manages the
 * waking up based on the resources available.  The acquire_inflight_cb should
 * inc the rqw->inflight if we have the ability to do so, or return false if not
 * and then we will sleep until the room becomes available.
 *
 * cleanup_cb is in case that we race with a waker and need to cleanup the
 * inflight count accordingly.
 */
void rq_qos_wait(struct rq_wait *rqw, void *private_data,
		 acquire_inflight_cb_t *acquire_inflight_cb,
		 cleanup_cb_t *cleanup_cb)
{
	struct rq_qos_wait_data data = {
		.wq = {
			.func	= rq_qos_wake_function,
			.entry	= LIST_HEAD_INIT(data.wq.entry),
		},
		.task = current,
		.rqw = rqw,
		.cb = acquire_inflight_cb,
		.private_data = private_data,
	};
	bool has_sleeper;

	has_sleeper = wq_has_sleeper(&rqw->wait);
	if (!has_sleeper && acquire_inflight_cb(rqw, private_data))
		return;

	has_sleeper = !prepare_to_wait_exclusive(&rqw->wait, &data.wq,
						 TASK_UNINTERRUPTIBLE);
	do {
		/* The memory barrier in set_task_state saves us here. */
		if (data.got_token)
			break;
		if (!has_sleeper && acquire_inflight_cb(rqw, private_data)) {
			finish_wait(&rqw->wait, &data.wq);

			/*
			 * We raced with wbt_wake_function() getting a token,
			 * which means we now have two. Put our local token
			 * and wake anyone else potentially waiting for one.
			 */
			smp_rmb();
			if (data.got_token)
				cleanup_cb(rqw, private_data);
			break;
		}
		io_schedule();
		has_sleeper = true;
		set_current_state(TASK_UNINTERRUPTIBLE);
	} while (1);
	finish_wait(&rqw->wait, &data.wq);
}

void rq_qos_exit(struct request_queue *q)
{
	/*
	 * queue must have been unregistered here, it is safe to iterate
	 * the list w/o lock
	 */
	while (q->rq_qos) {
		struct rq_qos *rqos = q->rq_qos;
		q->rq_qos = rqos->next;
		rqos->ops->exit(rqos);
	}
	blk_mq_debugfs_unregister_queue_rqos(q);
}

static struct rq_qos *rq_qos_by_name(struct request_queue *q,
		const char *name)
{
	struct rq_qos *rqos;

	for (rqos = q->rq_qos; rqos; rqos = rqos->next) {
		if (!rqos->ops->name)
			continue;

		if (!strncmp(rqos->ops->name, name,
					strlen(rqos->ops->name)))
			return rqos;
	}
	return NULL;
}

/*
 * After the pluggable blk-qos, rqos's life cycle become complicated,
 * as we may modify the rqos list there. Except for the places where
 * queue is not registered, there are following places may access rqos
 * list concurrently:
 * (1) normal IO path, can be serialized by queue freezing
 * (2) blkg_create, the .pd_init_fn() may access rqos, can be serialized
 *     by queue_lock.
 * (3) cgroup file, such as ioc_cost_model_write, rq_qos_get is for this
 *     case to keep the rqos alive.
 */
struct rq_qos *rq_qos_get(struct request_queue *q, int id)
{
	struct rq_qos *rqos;

	spin_lock_irq(&q->queue_lock);
	rqos = rq_qos_by_id(q, id);
	if (rqos && rqos->dying)
		rqos = NULL;
	if (rqos)
		refcount_inc(&rqos->ref);
	spin_unlock_irq(&q->queue_lock);
	return rqos;
}
EXPORT_SYMBOL_GPL(rq_qos_get);

void rq_qos_put(struct rq_qos *rqos)
{
	struct request_queue *q = rqos->q;

	spin_lock_irq(&q->queue_lock);
	refcount_dec(&rqos->ref);
	if (rqos->dying)
		wake_up(&rqos->waitq);
	spin_unlock_irq(&q->queue_lock);
}
EXPORT_SYMBOL_GPL(rq_qos_put);

void rq_qos_activate(struct request_queue *q,
		struct rq_qos *rqos, const struct rq_qos_ops *ops)
{
	struct rq_qos *pos;

	rqos->dying = false;
	refcount_set(&rqos->ref, 1);
	init_waitqueue_head(&rqos->waitq);
	rqos->id = ops->id;
	rqos->ops = ops;
	rqos->q = q;
	rqos->next = NULL;

	spin_lock_irq(&q->queue_lock);
	pos = q->rq_qos;
	if (pos) {
		while (pos->next)
			pos = pos->next;
		pos->next = rqos;
	} else {
		q->rq_qos = rqos;
	}
	spin_unlock_irq(&q->queue_lock);

	if (rqos->ops->debugfs_attrs)
		blk_mq_debugfs_register_rqos(rqos);

	if (ops->owner)
		__module_get(ops->owner);
}
EXPORT_SYMBOL_GPL(rq_qos_activate);

void rq_qos_deactivate(struct rq_qos *rqos)
{
	struct request_queue *q = rqos->q;
	struct rq_qos **cur;

	spin_lock_irq(&q->queue_lock);
	rqos->dying = true;
	/*
	 * Drain all of the usage of get/put_rqos()
	 */
	wait_event_lock_irq(rqos->waitq,
		refcount_read(&rqos->ref) == 1, q->queue_lock);
	for (cur = &q->rq_qos; *cur; cur = &(*cur)->next) {
		if (*cur == rqos) {
			*cur = rqos->next;
			break;
		}
	}
	spin_unlock_irq(&q->queue_lock);
	blk_mq_debugfs_unregister_rqos(rqos);

	if (rqos->ops->owner)
		module_put(rqos->ops->owner);
}
EXPORT_SYMBOL_GPL(rq_qos_deactivate);

static struct rq_qos_ops *rq_qos_op_find(const char *name)
{
	struct rq_qos_ops *pos;

	list_for_each_entry(pos, &rq_qos_list, node) {
		if (!strncmp(pos->name, name, strlen(pos->name)))
			return pos;
	}

	return NULL;
}

int rq_qos_register(struct rq_qos_ops *ops)
{
	int ret, start;

	mutex_lock(&rq_qos_mutex);

	if (rq_qos_op_find(ops->name)) {
		ret = -EEXIST;
		goto out;
	}

	if (ops->flags & RQOS_FLAG_CGRP_POL &&
	    nr_rqos_blkcg_pols >= (BLKCG_MAX_POLS - BLKCG_NON_RQOS_POLS)) {
		ret = -ENOSPC;
		goto out;
	}

	start = 1;
	ret = ida_simple_get(&rq_qos_ida, start, INT_MAX, GFP_KERNEL);
	if (ret < 0)
		goto out;

	if (ops->flags & RQOS_FLAG_CGRP_POL)
		nr_rqos_blkcg_pols++;

	ops->id = ret;
	ret = 0;
	INIT_LIST_HEAD(&ops->node);
	list_add_tail(&ops->node, &rq_qos_list);
out:
	mutex_unlock(&rq_qos_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(rq_qos_register);

void rq_qos_unregister(struct rq_qos_ops *ops)
{
	mutex_lock(&rq_qos_mutex);

	if (ops->flags & RQOS_FLAG_CGRP_POL)
		nr_rqos_blkcg_pols--;
	list_del_init(&ops->node);
	ida_simple_remove(&rq_qos_ida, ops->id);
	mutex_unlock(&rq_qos_mutex);
}
EXPORT_SYMBOL_GPL(rq_qos_unregister);

ssize_t queue_qos_show(struct request_queue *q, char *buf)
{
	struct rq_qos_ops *ops;
	struct rq_qos *rqos;
	int ret = 0;

	mutex_lock(&rq_qos_mutex);
	/*
	 * Show the policies in the order of being invoked.
	 * queue_lock is not needed here as the sysfs_lock is
	 * protected us from the queue_qos_store()
	 */
	for (rqos = q->rq_qos; rqos; rqos = rqos->next) {
		if (!rqos->ops->name)
			continue;
		ret += sprintf(buf + ret, "[%s] ", rqos->ops->name);
	}
	list_for_each_entry(ops, &rq_qos_list, node) {
		if (!rq_qos_by_name(q, ops->name))
			ret += sprintf(buf + ret, "%s ", ops->name);
	}

	ret--; /* overwrite the last space */
	ret += sprintf(buf + ret, "\n");
	mutex_unlock(&rq_qos_mutex);

	return ret;
}

static int rq_qos_switch(struct request_queue *q,
		const struct rq_qos_ops *ops,
		struct rq_qos *rqos)
{
	int ret;

	blk_mq_freeze_queue(q);
	if (!rqos) {
		ret = ops->init(q);
	} else {
		ops->exit(rqos);
		ret = 0;
	}
	blk_mq_unfreeze_queue(q);

	return ret;
}

ssize_t queue_qos_store(struct request_queue *q, const char *page,
			  size_t count)
{
	const struct rq_qos_ops *ops;
	struct rq_qos *rqos;
	const char *qosname;
	char *buf;
	bool add;
	int ret;

	if (!blk_queue_registered(q))
		return -ENOENT;

	buf = kstrdup(page, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf = strim(buf);
	if (buf[0] != '+' && buf[0] != '-') {
		ret = -EINVAL;
		goto out;
	}

	add = buf[0] == '+';
	qosname = buf + 1;

	rqos = rq_qos_by_name(q, qosname);
	if ((buf[0] == '+' && rqos)) {
		ret = -EEXIST;
		goto out;
	}

	if ((buf[0] == '-' && !rqos)) {
		ret = -ENODEV;
		goto out;
	}

	if (add) {
		mutex_lock(&rq_qos_mutex);
		ops = rq_qos_op_find(qosname);
		if (!ops) {
			/*
			 * module_init callback may request this mutex
			 */
			mutex_unlock(&rq_qos_mutex);
			request_module("%s", qosname);
			mutex_lock(&rq_qos_mutex);
			ops = rq_qos_op_find(qosname);
		}
		if (!ops) {
			ret = -EINVAL;
		} else if (ops->owner && !try_module_get(ops->owner)) {
			ops = NULL;
			ret = -EAGAIN;
		}
		mutex_unlock(&rq_qos_mutex);
		if (!ops)
			goto out;
	} else {
		ops = rqos->ops;
	}

	ret = rq_qos_switch(q, ops, add ? NULL : rqos);

	if (add)
		module_put(ops->owner);
out:
	kfree(buf);
	return ret ? ret : count;
}
