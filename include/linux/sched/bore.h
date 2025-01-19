
#include <linux/sched.h>
#include <linux/sched/cputime.h>

#ifndef _LINUX_SCHED_BORE_H
#define _LINUX_SCHED_BORE_H
#define SCHED_BORE_VERSION "5.9.6"

#ifdef CONFIG_SCHED_BORE
extern u8   __read_mostly sched_bore;
extern u8   __read_mostly sched_burst_exclude_kthreads;
extern u8   __read_mostly sched_burst_smoothness_long;
extern u8   __read_mostly sched_burst_smoothness_short;
extern u8   __read_mostly sched_burst_fork_atavistic;
extern u8   __read_mostly sched_burst_parity_threshold;
extern u8   __read_mostly sched_burst_penalty_offset;
extern uint __read_mostly sched_burst_penalty_scale;
extern uint __read_mostly sched_burst_cache_stop_count;
extern uint __read_mostly sched_burst_cache_lifetime;
extern uint __read_mostly sched_deadline_boost_mask;

extern void update_burst_score(struct sched_entity *se);
extern void update_burst_penalty(struct sched_entity *se);

extern void restart_burst(struct sched_entity *se);
extern void restart_burst_rescale_deadline(struct sched_entity *se);

extern int sched_bore_update_handler(const struct ctl_table *table, int write,
	void __user *buffer, size_t *lenp, loff_t *ppos);

extern void sched_clone_bore(
	struct task_struct *p, struct task_struct *parent, u64 clone_flags, u64 now);

extern void reset_task_bore(struct task_struct *p);
extern void sched_bore_init(void);

extern void reweight_entity(
	struct cfs_rq *cfs_rq, struct sched_entity *se, unsigned long weight);
#endif // CONFIG_SCHED_BORE
#endif // _LINUX_SCHED_BORE_H
