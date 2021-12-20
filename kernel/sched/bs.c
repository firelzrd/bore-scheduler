// SPDX-License-Identifier: GPL-2.0
/*
 * Burst-Oriented Response Enhancer (BORE) CPU Scheduler
 *  Copyright (C) 2021, Masahito Suzuki <firelzrd@gmail.com>
 */
#include "sched.h"
#include "pelt.h"
#include "fair_numa.h"
#include "bs.h"

#define BS_SCHED_MAX_TIME  0x8000000000ULL
#define BS_SCHED_MIN_SCORE 0
#define BS_SCHED_MAX_SCORE 0xFFFFFFFFFFFFFFFFULL

unsigned int __read_mostly sysctl_sched_timeslice_factor          = 200000;
unsigned int __read_mostly sysctl_sched_min_timeslice_factor      =  10000;
unsigned int __read_mostly sched_tick_timeslice_factor;

void bs_sched_update_internals(void)
{
	sched_tick_timeslice_factor = max(sysctl_sched_timeslice_factor,
	                                  sysctl_sched_min_timeslice_factor);
}

void __init sched_init_bs_sched(void) {bs_sched_update_internals();}

int sched_proc_update_handler(struct ctl_table *table, int write,
		void __user *buffer, size_t *lenp,
		loff_t *ppos)
{
	int ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);

	if (ret || !write)
		return ret;
	
	bs_sched_update_internals();

	return 0;
}

static void update_curr(struct cfs_rq *cfs_rq)
{
	struct sched_entity *curr = cfs_rq->curr;
	u64 now = sched_clock();
	u64 delta_exec;

	if (unlikely(!curr))
		return;

	delta_exec = now - curr->exec_start;
	if (unlikely((s64)delta_exec <= 0))
		return;

	curr->exec_start = now;
	curr->sum_exec_runtime += delta_exec;

	curr->bs_node.waiting_since = now;
	curr->bs_node.burst_time += delta_exec;
}

static void update_curr_fair(struct rq *rq)
{
	update_curr(cfs_rq_of(&rq->curr->se));
}

static inline u64
calc_score(u64 now, struct bs_node *bsn, bool wakeup)
{
	struct sched_entity *se = se_of(bsn);
	struct cfs_rq *cfs_rq = cfs_rq_of(se);
	u64 time_factor, burst_score, greed_score, power, resist;
	
	if(se == cfs_rq->curr) {
		if(unlikely(bsn->yield_flag)) return BS_SCHED_MIN_SCORE;
		if(wakeup)
			time_factor = max(
				sysctl_sched_timeslice_factor / (cfs_rq->nr_running - 1 || 1),
				sysctl_sched_min_timeslice_factor);
		else
			time_factor = sched_tick_timeslice_factor;
	} else {
		time_factor = now - bsn->waiting_since;
		if(unlikely(time_factor >= BS_SCHED_MAX_TIME)) return BS_SCHED_MAX_SCORE;
	}
	power = time_factor * scale_load_down(se->load.weight) << 7;
	
	greed_score = bsn->greed_score;
	// if the task has given up cputime at least once before, then add greed_score,
	// if the task has never given up cputime, then add bust_time on the resist.
	burst_score = bsn->burst_time + (greed_score || bsn->burst_time);
	resist = min(burst_score, BS_SCHED_MAX_TIME) + 1;

	return power / resist + 1;
}

/**
 * Does a have higher score than b?
 */
static inline bool
entity_before(u64 now, struct bs_node *a, struct bs_node *b, bool wakeup)
{
	u64 score_a = calc_score(now, a, wakeup);
	u64 score_b = calc_score(now, b, wakeup);
	
	return score_a > score_b;
}

static inline void reduce_burst_time(struct bs_node *bsn)
{
	u64 burst_score = min(bsn->burst_time, BS_SCHED_MAX_TIME);
	u64 greed_score = bsn->greed_score;
	bsn->burst_time = 0;
	if(greed_score)
		// the task has given up cputime at least once before
		bsn->greed_score = (greed_score + burst_score) >> 1 || 1;
	else
		// the first time the task is giving up cputime
		bsn->greed_score = burst_score;
}

static void __enqueue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	struct bs_node *bsn = &se->bs_node;

	bsn->next = bsn->prev = NULL;

	// if empty
	if (!cfs_rq->head) {
		cfs_rq->head	= bsn;
	}
	else {
		bsn->next	     = cfs_rq->head;
		cfs_rq->head->prev   = bsn;
		cfs_rq->head         = bsn;
	}
}

static void __dequeue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	struct bs_node *bsn = &se->bs_node;
	struct bs_node *prev, *next;

	// if only one se in rq
	if (cfs_rq->head->next == NULL) {
		cfs_rq->head = NULL;
	}
	// if it is the head
	else if (bsn == cfs_rq->head) {
		cfs_rq->head	   = cfs_rq->head->next;
		cfs_rq->head->prev = NULL;
	}
	// if in the middle
	else {
		prev = bsn->prev;
		next = bsn->next;

		prev->next = next;
		if (next)
			next->prev = prev;
	}
}

static inline void
enqueue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags)
{
	bool curr = cfs_rq->curr == se;

	update_curr(cfs_rq);

	account_entity_enqueue(cfs_rq, se);

	if (!curr)
		__enqueue_entity(cfs_rq, se);

	se->on_rq = 1;
}

static inline void
dequeue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags)
{
	update_curr(cfs_rq);
	
	reduce_burst_time(&se->bs_node);

	if (se != cfs_rq->curr)
		__dequeue_entity(cfs_rq, se);

	se->on_rq = 0;
	account_entity_dequeue(cfs_rq, se);
}

static void
enqueue_task_fair(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_entity *se = &p->se;
	struct cfs_rq *cfs_rq = cfs_rq_of(se);
	int idle_h_nr_running = task_has_idle_policy(p);

	if (!se->on_rq) {
		enqueue_entity(cfs_rq, se, flags);
		cfs_rq->h_nr_running++;
		cfs_rq->idle_h_nr_running += idle_h_nr_running;
	}

	add_nr_running(rq, 1);
}

static void dequeue_task_fair(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_entity *se = &p->se;
	struct cfs_rq *cfs_rq = cfs_rq_of(se);
	int idle_h_nr_running = task_has_idle_policy(p);

	dequeue_entity(cfs_rq, se, flags);

	cfs_rq->h_nr_running--;
	cfs_rq->idle_h_nr_running -= idle_h_nr_running;

	sub_nr_running(rq, 1);
}

static void yield_task_fair(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	struct cfs_rq *cfs_rq = task_cfs_rq(curr);

	reduce_burst_time(&curr->se.bs_node);
	
	/*
	 * Are we the only task in the tree?
	 */
	if (unlikely(rq->nr_running == 1))
		return;

	curr->se.bs_node.yield_flag = true;
	
	if (curr->policy != SCHED_BATCH) {
		update_rq_clock(rq);
		/*
		 * Update run-time statistics of the 'current'.
		 */
		update_curr(cfs_rq);
		/*
		 * Tell update_rq_clock() that we've just updated,
		 * so we don't do microscopic update in schedule()
		 * and double the fastpath cost.
		 */
		rq_clock_skip_update(rq);
	}
}

static bool yield_to_task_fair(struct rq *rq, struct task_struct *p)
{
	yield_task_fair(rq);
	return true;
}

static void
set_next_entity(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	if (se->on_rq)
		__dequeue_entity(cfs_rq, se);

	se->exec_start = sched_clock();
	cfs_rq->curr = se;
	se->prev_sum_exec_runtime = se->sum_exec_runtime;
}

static struct sched_entity *
pick_next_entity(struct cfs_rq *cfs_rq, struct sched_entity *curr)
{
	struct bs_node *bsn = cfs_rq->head;
	struct bs_node *next;
	u64 now = sched_clock();
	u64 highest_score, score;
	bool preempt_postponed;

	if (!bsn)
		return curr;

	preempt_postponed = bsn->preempt_postponed;
	highest_score = calc_score(now, bsn, preempt_postponed);
	next = bsn->next;
	while (next) {
		preempt_postponed |= next->preempt_postponed;
		//if (entity_before(now, next, bsn, preempt_postponed))
		score = calc_score(now, next, preempt_postponed);
		if (score > highest_score) {
			highest_score = score;
			bsn = next;
		}

		next = next->next;
	}

	//if (curr && !entity_before(now, bsn, &curr->bs_node, preempt_postponed))
	if (curr && (highest_score <= calc_score(now, &curr->bs_node, preempt_postponed)))
		return curr;

	bsn->preempt_postponed = false;
	return se_of(bsn);
}

struct task_struct *
pick_next_task_fair(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
	struct cfs_rq *cfs_rq = &rq->cfs;
	struct sched_entity *se;
	struct task_struct *p;
	int new_tasks;

again:
	if (!sched_fair_runnable(rq))
		goto idle;

	if (prev)
		put_prev_task(rq, prev);

	se = pick_next_entity(cfs_rq, NULL);
	set_next_entity(cfs_rq, se);

	p = task_of(se);

	if (prev)
		prev->se.bs_node.yield_flag = false;

done: __maybe_unused;
#ifdef CONFIG_SMP
	/*
	 * Move the next running task to the front of
	 * the list, so our cfs_tasks list becomes MRU
	 * one.
	 */
	list_move(&p->se.group_node, &rq->cfs_tasks);
#endif

	return p;

idle:
	if (!rf)
		return NULL;

	new_tasks = newidle_balance(rq, rf);

	/*
	 * Because newidle_balance() releases (and re-acquires) rq->lock, it is
	 * possible for any higher priority task to appear. In that case we
	 * must re-start the pick_next_entity() loop.
	 */
	if (new_tasks < 0)
		return RETRY_TASK;

	if (new_tasks > 0)
		goto again;

	/*
	 * rq is about to be idle, check if we need to update the
	 * lost_idle_time of clock_pelt
	 */
	update_idle_rq_clock_pelt(rq);

	return NULL;
}

static struct task_struct *__pick_next_task_fair(struct rq *rq)
{
	return pick_next_task_fair(rq, NULL, NULL);
}

#ifdef CONFIG_SMP
static struct task_struct *pick_task_fair(struct rq *rq)
{
	struct sched_entity *se;
	struct cfs_rq *cfs_rq = &rq->cfs;
	struct sched_entity *curr = cfs_rq->curr;

	if (!cfs_rq->nr_running)
		return NULL;

	/* When we pick for a remote RQ, we'll not have done put_prev_entity() */
	if (curr) {
		if (curr->on_rq)
			update_curr(cfs_rq);
		else
			curr = NULL;
	}

	se = pick_next_entity(cfs_rq, curr);

	return task_of(se);
}
#endif

static void put_prev_entity(struct cfs_rq *cfs_rq, struct sched_entity *prev)
{
	/*
	 * If still on the runqueue then deactivate_task()
	 * was not called and update_curr() has to be done:
	 */
	if (prev->on_rq)
		update_curr(cfs_rq);

	if (prev->on_rq)
		__enqueue_entity(cfs_rq, prev);

	cfs_rq->curr = NULL;
}

static void put_prev_task_fair(struct rq *rq, struct task_struct *prev)
{
	struct sched_entity *se = &prev->se;

	put_prev_entity(cfs_rq_of(se), se);
}

static void set_next_task_fair(struct rq *rq, struct task_struct *p, bool first)
{
	struct sched_entity *se = &p->se;
	struct cfs_rq *cfs_rq = cfs_rq_of(se);

#ifdef CONFIG_SMP
	if (task_on_rq_queued(p)) {
		/*
		 * Move the next running task to the front of the list, so our
		 * cfs_tasks list becomes MRU one.
		 */
		list_move(&se->group_node, &rq->cfs_tasks);
	}
#endif

	set_next_entity(cfs_rq, se);
}

static void
check_preempt_tick(struct cfs_rq *cfs_rq, struct sched_entity *curr)
{
	if (pick_next_entity(cfs_rq, curr) != curr)
#if !defined(CONFIG_PREEMPT_LAZY)
		resched_curr(rq_of(cfs_rq));
#else
		resched_curr_lazy(rq_of(cfs_rq));
#endif
}

static void
entity_tick(struct cfs_rq *cfs_rq, struct sched_entity *curr, int queued)
{
	update_curr(cfs_rq);

	if (cfs_rq->nr_running > 1)
		check_preempt_tick(cfs_rq, curr);
}

static void check_preempt_wakeup(struct rq *rq, struct task_struct *p, int wake_flags)
{
	struct task_struct *curr = rq->curr;
	struct sched_entity *se = &curr->se, *wse = &p->se;

	if (unlikely(se == wse))
		return;

	if (test_tsk_need_resched(curr))
		return;

	/* Idle tasks are by definition preempted by non-idle tasks. */
	if (unlikely(task_has_idle_policy(curr)) &&
	    likely(!task_has_idle_policy(p)))
		goto preempt;

	/*
	 * Batch and idle tasks do not preempt non-idle tasks (their preemption
	 * is driven by the tick):
	 */
	if (unlikely(p->policy != SCHED_NORMAL) || !sched_feat(WAKEUP_PREEMPTION))
		return;

	/*
	 * Lower priority tasks do not preempt higher ones
	 */
	if (p->prio > curr->prio)
		return;

	update_curr(cfs_rq_of(se));

	if (!entity_before(sched_clock(), &se->bs_node, &wse->bs_node, true))
		goto preempt;
	
	wse->bs_node.preempt_postponed = true;

	return;

preempt:
#if !defined(CONFIG_PREEMPT_LAZY)
	resched_curr(rq);
#else
	resched_curr_lazy(rq);
#endif
}

#ifdef CONFIG_SMP
static int
balance_fair(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
	if (rq->nr_running)
		return 1;

	return newidle_balance(rq, rf) != 0;
}

static int
select_task_rq_fair(struct task_struct *p, int prev_cpu, int wake_flags)
{
	struct rq *rq = cpu_rq(prev_cpu);
	unsigned int min_this = rq->nr_running;
	unsigned int min = rq->nr_running;
	int cpu, new_cpu = prev_cpu;

	for_each_online_cpu(cpu) {
		if (cpu_rq(cpu)->nr_running < min) {
			new_cpu = cpu;
			min = cpu_rq(cpu)->nr_running;
		}
	}

	if (min == min_this)
		return prev_cpu;

	return new_cpu;
}

static int
can_migrate_task(struct task_struct *p, int dst_cpu, struct rq *src_rq)
{
	if (task_running(src_rq, p))
		return 0;

	/* Disregard pcpu kthreads; they are where they need to be. */
	if (kthread_is_per_cpu(p))
		return 0;

	if (!cpumask_test_cpu(dst_cpu, p->cpus_ptr))
		return 0;

	return 1;
}

static void pull_from(struct rq *this_rq,
		      struct rq *src_rq,
		      struct rq_flags *src_rf,
		      struct task_struct *p)
{
	struct rq_flags rf;

	// detach task
	deactivate_task(src_rq, p, DEQUEUE_NOCLOCK);
	set_task_cpu(p, cpu_of(this_rq));

	// unlock src rq
	rq_unlock(src_rq, src_rf);

	// lock this rq
	rq_lock(this_rq, &rf);
	update_rq_clock(this_rq);

	activate_task(this_rq, p, ENQUEUE_NOCLOCK);
	check_preempt_curr(this_rq, p, 0);

	// unlock this rq
	rq_unlock(this_rq, &rf);

	local_irq_restore(src_rf->flags);
}

static int move_task(struct rq *this_rq, struct rq *src_rq,
			struct rq_flags *src_rf)
{
	struct cfs_rq *src_cfs_rq = &src_rq->cfs;
	struct task_struct *p;
	struct bs_node *bsn = src_cfs_rq->head;
	int moved = 0;

	while (bsn) {
		p = task_of(se_of(bsn));
		if (can_migrate_task(p, cpu_of(this_rq), src_rq)) {
			pull_from(this_rq, src_rq, src_rf, p);
			moved = 1;
			break;
		}

		bsn = bsn->next;
	}

	if (!moved) {
		rq_unlock(src_rq, src_rf);
		local_irq_restore(src_rf->flags);
	}

	return moved;
}

static int newidle_balance(struct rq *this_rq, struct rq_flags *rf)
{
	int this_cpu = this_rq->cpu;
	struct rq *src_rq;
	int src_cpu = -1, cpu;
	int pulled_task = 0;
	unsigned int max = 0;
	struct rq_flags src_rf;

	/*
	 * We must set idle_stamp _before_ calling idle_balance(), such that we
	 * measure the duration of idle_balance() as idle time.
	 */
	this_rq->idle_stamp = rq_clock(this_rq);

	/*
	 * Do not pull tasks towards !active CPUs...
	 */
	if (!cpu_active(this_cpu))
		return 0;

	rq_unpin_lock(this_rq, rf);
	raw_spin_unlock(&this_rq->__lock);

	for_each_online_cpu(cpu) {
		/*
		 * Stop searching for tasks to pull if there are
		 * now runnable tasks on this rq.
		 */
		if (this_rq->nr_running > 0)
			goto out;

		if (cpu == this_cpu)
			continue;

		src_rq = cpu_rq(cpu);

		if (src_rq->nr_running < 2)
			continue;

		if (src_rq->nr_running > max) {
			max = src_rq->nr_running;
			src_cpu = cpu;
		}
	}

	if (src_cpu != -1) {
		src_rq = cpu_rq(src_cpu);

		rq_lock_irqsave(src_rq, &src_rf);
		update_rq_clock(src_rq);

		if (src_rq->nr_running < 2) {
			rq_unlock(src_rq, &src_rf);
			local_irq_restore(src_rf.flags);
		} else {
			pulled_task = move_task(this_rq, src_rq, &src_rf);
		}
	}

out:
	raw_spin_lock(&this_rq->__lock);

	/*
	 * While browsing the domains, we released the rq lock, a task could
	 * have been enqueued in the meantime. Since we're not going idle,
	 * pretend we pulled a task.
	 */
	if (this_rq->cfs.h_nr_running && !pulled_task)
		pulled_task = 1;

	/* Is there a task of a high priority class? */
	if (this_rq->nr_running != this_rq->cfs.h_nr_running)
		pulled_task = -1;

	if (pulled_task)
		this_rq->idle_stamp = 0;

	rq_repin_lock(this_rq, rf);

	return pulled_task;
}

static inline int on_null_domain(struct rq *rq)
{
	return unlikely(!rcu_dereference_sched(rq->sd));
}

void trigger_load_balance(struct rq *this_rq)
{
	int this_cpu = cpu_of(this_rq);
	int cpu;
	unsigned int max, min;
	struct rq *max_rq, *min_rq, *c_rq;
	struct rq_flags src_rf;

	if (this_cpu != 0)
		return;

	max = min = this_rq->nr_running;
	max_rq = min_rq = this_rq;

	for_each_online_cpu(cpu) {
		c_rq = cpu_rq(cpu);

		/*
		 * Don't need to rebalance while attached to NULL domain or
		 * runqueue CPU is not active
		 */
		if (unlikely(on_null_domain(c_rq) || !cpu_active(cpu)))
			continue;

		if (c_rq->nr_running < min) {
			min = c_rq->nr_running;
			min_rq = c_rq;
		}

		if (c_rq->nr_running > max) {
			max = c_rq->nr_running;
			max_rq = c_rq;
		}
	}

	if (min_rq == max_rq || max - min < 2)
		return;

	rq_lock_irqsave(max_rq, &src_rf);
	update_rq_clock(max_rq);

	if (max_rq->nr_running < 2) {
		rq_unlock(max_rq, &src_rf);
		local_irq_restore(src_rf.flags);
		return;
	}

	move_task(min_rq, max_rq, &src_rf);
}

void update_group_capacity(struct sched_domain *sd, int cpu) {}
#endif /* CONFIG_SMP */

static void task_tick_fair(struct rq *rq, struct task_struct *curr, int queued)
{
	struct sched_entity *se = &curr->se;
	struct cfs_rq *cfs_rq = cfs_rq_of(se);

	entity_tick(cfs_rq, se, queued);

	if (static_branch_unlikely(&sched_numa_balancing))
		task_tick_numa(rq, curr);
}

static void task_fork_fair(struct task_struct *p)
{
	struct cfs_rq *cfs_rq;
	struct sched_entity *curr;
	struct rq *rq = this_rq();
	struct rq_flags rf;

	p->se.bs_node.greed_score = 0;
	p->se.bs_node.burst_time = 0;

	rq_lock(rq, &rf);
	update_rq_clock(rq);

	cfs_rq = task_cfs_rq(current);
	curr = cfs_rq->curr;
	if (curr)
		update_curr(cfs_rq);

	rq_unlock(rq, &rf);
}

/*
 * All the scheduling class methods:
 */
DEFINE_SCHED_CLASS(fair) = {

	.enqueue_task		= enqueue_task_fair,
	.dequeue_task		= dequeue_task_fair,
	.yield_task		= yield_task_fair,
	.yield_to_task		= yield_to_task_fair,

	.check_preempt_curr	= check_preempt_wakeup,

	.pick_next_task		= __pick_next_task_fair,
	.put_prev_task		= put_prev_task_fair,
	.set_next_task          = set_next_task_fair,

#ifdef CONFIG_SMP
	.balance		= balance_fair,
	.pick_task		= pick_task_fair,
	.select_task_rq		= select_task_rq_fair,
	.migrate_task_rq	= migrate_task_rq_fair,

	.rq_online		= rq_online_fair,
	.rq_offline		= rq_offline_fair,

	.task_dead		= task_dead_fair,
	.set_cpus_allowed	= set_cpus_allowed_common,
#endif

	.task_tick		= task_tick_fair,
	.task_fork		= task_fork_fair,

	.prio_changed		= prio_changed_fair,
	.switched_from		= switched_from_fair,
	.switched_to		= switched_to_fair,

	.get_rr_interval	= get_rr_interval_fair,

	.update_curr		= update_curr_fair,
};
