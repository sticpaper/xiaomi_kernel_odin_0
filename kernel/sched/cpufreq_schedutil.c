// SPDX-License-Identifier: GPL-2.0
/*
 * CPUFreq governor based on scheduler-provided CPU utilization data.
 *
 * Copyright (C) 2016, Intel Corporation
 * Author: Rafael J. Wysocki <rafael.j.wysocki@intel.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "sched.h"

#include <linux/sched/cpufreq.h>
#include <trace/events/power.h>
#include <linux/sched/sysctl.h>
#include <trace/hooks/sched.h>
#include <linux/hwui_mon.h>

#define TARGET_FRAME_TIME 4000
#define DEFAULT_CPU0_EFFICIENT_FREQ 1209600
#define DEFAULT_CPU4_EFFICIENT_FREQ 960000
#define DEFAULT_CPU7_EFFICIENT_FREQ 844800

#define IOWAIT_BOOST_MIN	(SCHED_CAPACITY_SCALE / 8)

struct sugov_tunables {
	struct gov_attr_set	attr_set;
	unsigned int		up_rate_limit_us;
	unsigned int		down_rate_limit_us;
	unsigned int		hispeed_load;
	unsigned int		hispeed_freq;
	unsigned int		rtg_boost_freq;
	unsigned int		efficient_freq;
	bool			frame_aware;
	bool			pl;
};

struct sugov_policy {
	struct cpufreq_policy	*policy;

	u64 last_ws;
	u64 curr_cycles;
	u64 last_cyc_update_time;
	unsigned long avg_cap;
	struct sugov_tunables	*tunables;
	struct list_head	tunables_hook;
	unsigned long hispeed_util;
	unsigned long rtg_boost_util;
	unsigned long max;

	raw_spinlock_t		update_lock;	/* For shared policies */
	u64			last_freq_update_time;
	s64			min_rate_limit_ns;
	s64			up_rate_delay_ns;
	s64			down_rate_delay_ns;
	unsigned int		next_freq;
	unsigned int		cached_raw_freq;
	struct list_head	frame_boost_info;

	/* The next fields are only needed if fast switch cannot be used: */
	struct			irq_work irq_work;
	struct			kthread_work work;
	struct			mutex work_lock;
	struct			kthread_worker worker;
	struct task_struct	*thread;
	bool			work_in_progress;

	bool			limits_changed;
	bool			need_freq_update;
	unsigned int		flags;
};

struct sugov_cpu {
	struct update_util_data	update_util;
	struct sugov_policy	*sg_policy;
	unsigned int		cpu;

	bool			iowait_boost_pending;
	unsigned int		iowait_boost;
	u64			last_update;

	struct walt_cpu_load	walt_load;

	unsigned long util;
	unsigned int flags;

	unsigned long		bw_dl;
	unsigned long		max;

	/* The field below is for single-CPU policies only: */
#ifdef CONFIG_NO_HZ_COMMON
	unsigned long		saved_idle_calls;
#endif
};

struct frame_boost_info {
	struct list_head list;
	unsigned int freq;
	u64 expiration;
};
static struct kmem_cache *kmem_frame_boost_pool;

static DEFINE_PER_CPU(struct sugov_cpu, sugov_cpu);
static unsigned int stale_ns;
static DEFINE_PER_CPU(struct sugov_tunables *, cached_tunables);

/************************ Governor internals ***********************/

static inline void update_lock(struct sugov_policy *sg_policy)
{
	if (policy_is_shared(sg_policy->policy))
		raw_spin_lock(&sg_policy->update_lock);
}

static inline void update_unlock(struct sugov_policy *sg_policy)
{
	if (policy_is_shared(sg_policy->policy))
		raw_spin_unlock(&sg_policy->update_lock);
}

static void sugov_push_frame_boost(struct sugov_policy *sg_policy,
        struct frame_boost_info *info)
{
	update_lock(sg_policy);
	list_add_tail(&info->list, &sg_policy->frame_boost_info);
	update_unlock(sg_policy);
}

static void sugov_clean_frame_boost(struct sugov_policy *sg_policy)
{
	struct frame_boost_info *boost;

	update_lock(sg_policy);
	while (true) {
		boost = list_first_entry_or_null(&sg_policy->frame_boost_info,
		        struct frame_boost_info, list);
		if (!boost)
			break;
		list_del(&boost->list);
		kmem_cache_free(kmem_frame_boost_pool, boost);
	}
	update_unlock(sg_policy);
}

/**
 * Must called with update_lock held if share policy.
 */
static void __sugov_expire_frame_boost(struct sugov_policy *sg_policy, u64 time)
{
	struct frame_boost_info *boost;

	// Clean expired boosts.
	while (true) {
		boost = list_first_entry_or_null(&sg_policy->frame_boost_info,
		        struct frame_boost_info, list);
		if (!boost || boost->expiration > time)
			break;
		list_del(&boost->list);
		kmem_cache_free(kmem_frame_boost_pool, boost);
	}
}

/**
 * Must called with update_lock held if share policy.
 */
static unsigned int __sugov_get_frame_boost(struct sugov_policy *sg_policy)
{
	struct frame_boost_info *boost;
	unsigned int max_freq = 0;

	if (list_empty(&sg_policy->frame_boost_info))
		return 0;

	list_for_each_entry(boost, &sg_policy->frame_boost_info, list) {
		max_freq = max(max_freq, boost->freq);
	}

	return max_freq;
}

static bool sugov_should_update_freq(struct sugov_policy *sg_policy, u64 time)
{
	s64 delta_ns;

	/*
	 * Since cpufreq_update_util() is called with rq->lock held for
	 * the @target_cpu, our per-CPU data is fully serialized.
	 *
	 * However, drivers cannot in general deal with cross-CPU
	 * requests, so while get_next_freq() will work, our
	 * sugov_update_commit() call may not for the fast switching platforms.
	 *
	 * Hence stop here for remote requests if they aren't supported
	 * by the hardware, as calculating the frequency is pointless if
	 * we cannot in fact act on it.
	 *
	 * This is needed on the slow switching platforms too to prevent CPUs
	 * going offline from leaving stale IRQ work items behind.
	 */
	if (!cpufreq_this_cpu_can_update(sg_policy->policy))
		return false;

	if (unlikely(sg_policy->limits_changed)) {
		sg_policy->limits_changed = false;
		sg_policy->need_freq_update = true;
		return true;
	}

	if (sg_policy->flags & SCHED_CPUFREQ_SKIP_LIMITS)
		return true;

	/* No need to recalculate next freq for min_rate_limit_us
	 * at least. However we might still decide to further rate
	 * limit once frequency change direction is decided, according
	 * to the separate rate limits.
	 */

	delta_ns = time - sg_policy->last_freq_update_time;
	return delta_ns >= sg_policy->min_rate_limit_ns;
}

static bool sugov_up_down_rate_limit(struct sugov_policy *sg_policy, u64 time,
				     unsigned int next_freq)
{
	s64 delta_ns;

	delta_ns = time - sg_policy->last_freq_update_time;

	if (sg_policy->flags & SCHED_CPUFREQ_SKIP_LIMITS)
		return false;

	if (next_freq > sg_policy->next_freq &&
	    delta_ns < sg_policy->up_rate_delay_ns)
			return true;

	if (next_freq < sg_policy->next_freq &&
	    delta_ns < sg_policy->down_rate_delay_ns)
			return true;

	return false;
}

static inline bool use_pelt(void)
{
#ifdef CONFIG_SCHED_WALT
	return false;
#else
	return true;
#endif
}

static inline bool conservative_pl(void)
{
#ifdef CONFIG_SCHED_WALT
	return sysctl_sched_conservative_pl;
#else
	return false;
#endif
}

static bool sugov_update_next_freq(struct sugov_policy *sg_policy, u64 time,
				   unsigned int next_freq)
{
	if (sg_policy->next_freq == next_freq)
		return false;

	if (sugov_up_down_rate_limit(sg_policy, time, next_freq))
		return false;

	sg_policy->next_freq = next_freq;
	sg_policy->last_freq_update_time = time;

	return true;
}

static unsigned long freq_to_util(struct sugov_policy *sg_policy,
				  unsigned int freq)
{
	return mult_frac(sg_policy->max, freq,
			 sg_policy->policy->cpuinfo.max_freq);
}

#define KHZ 1000
static void sugov_track_cycles(struct sugov_policy *sg_policy,
				unsigned int prev_freq,
				u64 upto)
{
	u64 delta_ns, cycles;
	u64 next_ws = sg_policy->last_ws + sched_ravg_window;

	if (use_pelt())
		return;

	upto = min(upto, next_ws);
	/* Track cycles in current window */
	delta_ns = upto - sg_policy->last_cyc_update_time;
	delta_ns *= prev_freq;
	do_div(delta_ns, (NSEC_PER_SEC / KHZ));
	cycles = delta_ns;
	sg_policy->curr_cycles += cycles;
	sg_policy->last_cyc_update_time = upto;
}

static void sugov_calc_avg_cap(struct sugov_policy *sg_policy, u64 curr_ws,
				unsigned int prev_freq)
{
	u64 last_ws = sg_policy->last_ws;
	unsigned int avg_freq;

	if (use_pelt())
		return;

	BUG_ON(curr_ws < last_ws);
	if (curr_ws <= last_ws)
		return;

	/* If we skipped some windows */
	if (curr_ws > (last_ws + sched_ravg_window)) {
		avg_freq = prev_freq;
		/* Reset tracking history */
		sg_policy->last_cyc_update_time = curr_ws;
	} else {
		sugov_track_cycles(sg_policy, prev_freq, curr_ws);
		avg_freq = sg_policy->curr_cycles;
		avg_freq /= sched_ravg_window / (NSEC_PER_SEC / KHZ);
	}
	sg_policy->avg_cap = freq_to_util(sg_policy, avg_freq);
	sg_policy->curr_cycles = 0;
	sg_policy->last_ws = curr_ws;
}

static void sugov_fast_switch(struct sugov_policy *sg_policy, u64 time,
			      unsigned int next_freq)
{
	struct cpufreq_policy *policy = sg_policy->policy;
	int cpu;

	if (!sugov_update_next_freq(sg_policy, time, next_freq))
		return;

	sugov_track_cycles(sg_policy, sg_policy->policy->cur, time);
	next_freq = cpufreq_driver_fast_switch(policy, next_freq);
	if (!next_freq)
		return;

	policy->cur = next_freq;

	if (trace_cpu_frequency_enabled()) {
		for_each_cpu(cpu, policy->cpus)
			trace_cpu_frequency(next_freq, cpu);
	}
}

static void sugov_deferred_update(struct sugov_policy *sg_policy, u64 time,
				  unsigned int next_freq)
{
	if (!sugov_update_next_freq(sg_policy, time, next_freq))
		return;

	if (use_pelt())
		sg_policy->work_in_progress = true;
	walt_irq_work_queue(&sg_policy->irq_work);
}

#define TARGET_LOAD 80
/**
 * get_next_freq - Compute a new frequency for a given cpufreq policy.
 * @sg_policy: schedutil policy object to compute the new frequency for.
 * @util: Current CPU utilization.
 * @max: CPU capacity.
 *
 * If the utilization is frequency-invariant, choose the new frequency to be
 * proportional to it, that is
 *
 * next_freq = C * max_freq * util / max
 *
 * Otherwise, approximate the would-be frequency-invariant utilization by
 * util_raw * (curr_freq / max_freq) which leads to
 *
 * next_freq = C * curr_freq * util_raw / max
 *
 * Take C = 1.25 for the frequency tipping point at (util / max) = 0.8.
 *
 * The lowest driver-supported frequency which is equal or greater than the raw
 * next_freq (as calculated above) is returned, subject to policy min/max and
 * cpufreq driver limitations.
 */
static unsigned int get_next_freq(struct sugov_policy *sg_policy,
				  unsigned long util, unsigned long max, u64 time)
{
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned int freq;
        unsigned int idx, l_freq, h_freq;
	unsigned long next_freq = 0;
	__sugov_expire_frame_boost(sg_policy, time);
	if (sg_policy->tunables->frame_aware) {
		freq = __sugov_get_frame_boost(sg_policy);
		if (freq) {
			goto out;
		}
	}
	freq = arch_scale_freq_invariant() ?
			policy->cpuinfo.max_freq : policy->cur;
	trace_android_vh_map_util_freq(util, freq, max, &next_freq);
	if (next_freq)
		freq = next_freq;
	else
		freq = map_util_freq(util, freq, max);

	trace_sugov_next_freq(policy->cpu, util, max, freq);

	if (sg_policy->tunables->frame_aware)
		freq = min(freq, sg_policy->tunables->efficient_freq);
out:

	if (freq == sg_policy->cached_raw_freq && !sg_policy->need_freq_update)
		return sg_policy->next_freq;

	sg_policy->need_freq_update = false;
	sg_policy->cached_raw_freq = freq;
	l_freq = cpufreq_driver_resolve_freq(policy, freq);
	idx = cpufreq_frequency_table_target(policy, freq, CPUFREQ_RELATION_H);
	h_freq = policy->freq_table[idx].frequency;
	h_freq = clamp(h_freq, policy->min, policy->max);
	if (l_freq <= h_freq || l_freq == policy->min)
		return l_freq;

	/*
	 * Use the frequency step below if the calculated frequency is <20%
	 * higher than it.
	 */
	if (mult_frac(100, freq - h_freq, l_freq - h_freq) < 20)
		return h_freq;

	return l_freq;
}

/*
 * This function computes an effective utilization for the given CPU, to be
 * used for frequency selection given the linear relation: f = u * f_max.
 *
 * The scheduler tracks the following metrics:
 *
 *   cpu_util_{cfs,rt,dl,irq}()
 *   cpu_bw_dl()
 *
 * Where the cfs,rt and dl util numbers are tracked with the same metric and
 * synchronized windows and are thus directly comparable.
 *
 * The cfs,rt,dl utilization are the running times measured with rq->clock_task
 * which excludes things like IRQ and steal-time. These latter are then accrued
 * in the irq utilization.
 *
 * The DL bandwidth number otoh is not a measured metric but a value computed
 * based on the task model parameters and gives the minimal utilization
 * required to meet deadlines.
 */
unsigned long schedutil_cpu_util(int cpu, unsigned long util_cfs,
				 unsigned long max, enum schedutil_type type,
				 struct task_struct *p)
{
	unsigned long dl_util, util, irq;
	struct rq *rq = cpu_rq(cpu);

	if (!uclamp_is_used() && sched_feat(SUGOV_RT_MAX_FREQ) &&
	    type == FREQUENCY_UTIL && rt_rq_is_runnable(&rq->rt)) {
		return max;
	}

	/*
	 * Early check to see if IRQ/steal time saturates the CPU, can be
	 * because of inaccuracies in how we track these -- see
	 * update_irq_load_avg().
	 */
	irq = cpu_util_irq(rq);
	if (unlikely(irq >= max))
		return max;

	/*
	 * Because the time spend on RT/DL tasks is visible as 'lost' time to
	 * CFS tasks and we use the same metric to track the effective
	 * utilization (PELT windows are synchronized) we can directly add them
	 * to obtain the CPU's actual utilization.
	 *
	 * CFS and RT utilization can be boosted or capped, depending on
	 * utilization clamp constraints requested by currently RUNNABLE
	 * tasks.
	 * When there are no CFS RUNNABLE tasks, clamps are released and
	 * frequency will be gracefully reduced with the utilization decay.
	 */
	util = util_cfs + cpu_util_rt(rq);
	if (type == FREQUENCY_UTIL)
		util = uclamp_rq_util_with(rq, util, p);

	dl_util = cpu_util_dl(rq);

	/*
	 * For frequency selection we do not make cpu_util_dl() a permanent part
	 * of this sum because we want to use cpu_bw_dl() later on, but we need
	 * to check if the CFS+RT+DL sum is saturated (ie. no idle time) such
	 * that we select f_max when there is no idle time.
	 *
	 * NOTE: numerical errors or stop class might cause us to not quite hit
	 * saturation when we should -- something for later.
	 */
	if (util + dl_util >= max)
		return max;

	/*
	 * OTOH, for energy computation we need the estimated running time, so
	 * include util_dl and ignore dl_bw.
	 */
	if (type == ENERGY_UTIL)
		util += dl_util;

	/*
	 * There is still idle time; further improve the number by using the
	 * irq metric. Because IRQ/steal time is hidden from the task clock we
	 * need to scale the task numbers:
	 *
	 *              max - irq
	 *   U' = irq + --------- * U
	 *                 max
	 */
	util = scale_irq_capacity(util, irq, max);
	util += irq;

	/*
	 * Bandwidth required by DEADLINE must always be granted while, for
	 * FAIR and RT, we use blocked utilization of IDLE CPUs as a mechanism
	 * to gracefully reduce the frequency when no tasks show up for longer
	 * periods of time.
	 *
	 * Ideally we would like to set bw_dl as min/guaranteed freq and util +
	 * bw_dl as requested freq. However, cpufreq is not yet ready for such
	 * an interface. So, we only do the latter for now.
	 */
	if (type == FREQUENCY_UTIL)
		util += cpu_bw_dl(rq);

	return min(max, util);
}

static unsigned long sugov_get_util(struct sugov_cpu *sg_cpu)
{
	struct rq *rq = cpu_rq(sg_cpu->cpu);
	unsigned long max = arch_scale_cpu_capacity(sg_cpu->cpu);
	unsigned long util;

	sg_cpu->max = max;
	sg_cpu->bw_dl = cpu_bw_dl(rq);

#ifdef CONFIG_SCHED_WALT
	util = cpu_util_freq_walt(sg_cpu->cpu, &sg_cpu->walt_load);

	return uclamp_rq_util_with(rq, util, NULL);
#else
	util = cpu_util_cfs(rq);

	return schedutil_cpu_util(sg_cpu->cpu, util, max, FREQUENCY_UTIL, NULL);
#endif
}

/**
 * sugov_iowait_reset() - Reset the IO boost status of a CPU.
 * @sg_cpu: the sugov data for the CPU to boost
 * @time: the update time from the caller
 * @set_iowait_boost: true if an IO boost has been requested
 *
 * The IO wait boost of a task is disabled after a tick since the last update
 * of a CPU. If a new IO wait boost is requested after more then a tick, then
 * we enable the boost starting from IOWAIT_BOOST_MIN, which improves energy
 * efficiency by ignoring sporadic wakeups from IO.
 */
static bool sugov_iowait_reset(struct sugov_cpu *sg_cpu, u64 time,
			       bool set_iowait_boost)
{
	s64 delta_ns = time - sg_cpu->last_update;

	/* Reset boost only if a tick has elapsed since last request */
	if (delta_ns <= TICK_NSEC)
		return false;

	sg_cpu->iowait_boost = set_iowait_boost ? IOWAIT_BOOST_MIN : 0;
	sg_cpu->iowait_boost_pending = set_iowait_boost;

	return true;
}

/**
 * sugov_iowait_boost() - Updates the IO boost status of a CPU.
 * @sg_cpu: the sugov data for the CPU to boost
 * @time: the update time from the caller
 * @flags: SCHED_CPUFREQ_IOWAIT if the task is waking up after an IO wait
 *
 * Each time a task wakes up after an IO operation, the CPU utilization can be
 * boosted to a certain utilization which doubles at each "frequent and
 * successive" wakeup from IO, ranging from IOWAIT_BOOST_MIN to the utilization
 * of the maximum OPP.
 *
 * To keep doubling, an IO boost has to be requested at least once per tick,
 * otherwise we restart from the utilization of the minimum OPP.
 */
static void sugov_iowait_boost(struct sugov_cpu *sg_cpu, u64 time,
			       unsigned int flags)
{
	bool set_iowait_boost = flags & SCHED_CPUFREQ_IOWAIT;

	/* Reset boost if the CPU appears to have been idle enough */
	if (sg_cpu->iowait_boost &&
	    sugov_iowait_reset(sg_cpu, time, set_iowait_boost))
		return;

	/* Boost only tasks waking up after IO */
	if (!set_iowait_boost)
		return;

	/* Ensure boost doubles only one time at each request */
	if (sg_cpu->iowait_boost_pending)
		return;
	sg_cpu->iowait_boost_pending = true;

	/* Double the boost at each request */
	if (sg_cpu->iowait_boost) {
		sg_cpu->iowait_boost =
			min_t(unsigned int, sg_cpu->iowait_boost << 1, SCHED_CAPACITY_SCALE);
		return;
	}

	/* First wakeup after IO: start with minimum boost */
	sg_cpu->iowait_boost = IOWAIT_BOOST_MIN;
}

/**
 * sugov_iowait_apply() - Apply the IO boost to a CPU.
 * @sg_cpu: the sugov data for the cpu to boost
 * @time: the update time from the caller
 * @util: the utilization to (eventually) boost
 * @max: the maximum value the utilization can be boosted to
 *
 * A CPU running a task which woken up after an IO operation can have its
 * utilization boosted to speed up the completion of those IO operations.
 * The IO boost value is increased each time a task wakes up from IO, in
 * sugov_iowait_apply(), and it's instead decreased by this function,
 * each time an increase has not been requested (!iowait_boost_pending).
 *
 * A CPU which also appears to have been idle for at least one tick has also
 * its IO boost utilization reset.
 *
 * This mechanism is designed to boost high frequently IO waiting tasks, while
 * being more conservative on tasks which does sporadic IO operations.
 */
static unsigned long sugov_iowait_apply(struct sugov_cpu *sg_cpu, u64 time,
					unsigned long util, unsigned long max)
{
	unsigned long boost;

	/* No boost currently required */
	if (!sg_cpu->iowait_boost)
		return util;

	/* Reset boost if the CPU appears to have been idle enough */
	if (sugov_iowait_reset(sg_cpu, time, false))
		return util;

	if (!sg_cpu->iowait_boost_pending) {
		/*
		 * No boost pending; reduce the boost value.
		 */
		sg_cpu->iowait_boost >>= 1;
		if (sg_cpu->iowait_boost < IOWAIT_BOOST_MIN) {
			sg_cpu->iowait_boost = 0;
			return util;
		}
	}

	sg_cpu->iowait_boost_pending = false;

	/*
	 * @util is already in capacity scale; convert iowait_boost
	 * into the same scale so we can compare.
	 */
	boost = (sg_cpu->iowait_boost * max) >> SCHED_CAPACITY_SHIFT;
	return max(boost, util);
}

#ifdef CONFIG_NO_HZ_COMMON
static bool sugov_cpu_is_busy(struct sugov_cpu *sg_cpu)
{
	unsigned long idle_calls = tick_nohz_get_idle_calls_cpu(sg_cpu->cpu);
	bool ret = idle_calls == sg_cpu->saved_idle_calls;

	sg_cpu->saved_idle_calls = idle_calls;
	return ret;
}
#else
static inline bool sugov_cpu_is_busy(struct sugov_cpu *sg_cpu) { return false; }
#endif /* CONFIG_NO_HZ_COMMON */

#define NL_RATIO 75
#define DEFAULT_HISPEED_LOAD 90
#define DEFAULT_CPU0_RTG_BOOST_FREQ 1000000
#define DEFAULT_CPU4_RTG_BOOST_FREQ 0
#define DEFAULT_CPU7_RTG_BOOST_FREQ 0
static void sugov_walt_adjust(struct sugov_cpu *sg_cpu, unsigned long *util,
			      unsigned long *max)
{
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	bool is_migration = sg_cpu->flags & SCHED_CPUFREQ_INTERCLUSTER_MIG;
	bool is_rtg_boost = sg_cpu->walt_load.rtgb_active;
	unsigned long nl = sg_cpu->walt_load.nl;
	unsigned long cpu_util = sg_cpu->util;
	bool is_hiload;
	unsigned long pl = sg_cpu->walt_load.pl;

	if (use_pelt())
		return;

	if (is_rtg_boost)
		*util = max(*util, sg_policy->rtg_boost_util);

	is_hiload = (cpu_util >= mult_frac(sg_policy->avg_cap,
					   sg_policy->tunables->hispeed_load,
					   100));

	if (is_hiload && !is_migration)
		*util = max(*util, sg_policy->hispeed_util);

	if (is_hiload && nl >= mult_frac(cpu_util, NL_RATIO, 100))
		*util = *max;

	if (sg_policy->tunables->pl) {
		if (conservative_pl())
			pl = mult_frac(pl, TARGET_LOAD, 100);
		*util = max(*util, pl);
	}
}

/*
 * Make sugov_should_update_freq() ignore the rate limit when DL
 * has increased the utilization.
 */
static inline void ignore_dl_rate_limit(struct sugov_cpu *sg_cpu, struct sugov_policy *sg_policy)
{
	if (cpu_bw_dl(cpu_rq(sg_cpu->cpu)) > sg_cpu->bw_dl)
		sg_policy->limits_changed = true;
}

static inline unsigned long target_util(struct sugov_policy *sg_policy,
				  unsigned int freq)
{
	unsigned long util;

	util = freq_to_util(sg_policy, freq);
	util = mult_frac(util, TARGET_LOAD, 100);
	return util;
}

static void sugov_update_single(struct update_util_data *hook, u64 time,
				unsigned int flags)
{
	struct sugov_cpu *sg_cpu = container_of(hook, struct sugov_cpu, update_util);
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	unsigned long util, max, hs_util, boost_util;
	unsigned int next_f;
	bool busy;

	if (!sg_policy->tunables->pl && flags & SCHED_CPUFREQ_PL)
		return;
	sg_policy->flags = flags;
	sugov_iowait_boost(sg_cpu, time, flags);
	sg_cpu->last_update = time;

	ignore_dl_rate_limit(sg_cpu, sg_policy);

	if (!sugov_should_update_freq(sg_policy, time))
		return;

	/* Limits may have changed, don't skip frequency update */
	busy = use_pelt() && !sg_policy->need_freq_update &&
		sugov_cpu_is_busy(sg_cpu);

	sg_cpu->util = util = sugov_get_util(sg_cpu);
	max = sg_cpu->max;
	sg_cpu->flags = flags;

	if (sg_policy->max != max) {
		sg_policy->max = max;
		hs_util = target_util(sg_policy,
				       sg_policy->tunables->hispeed_freq);
		sg_policy->hispeed_util = hs_util;

		boost_util = target_util(sg_policy,
				    sg_policy->tunables->rtg_boost_freq);
		sg_policy->rtg_boost_util = boost_util;
	}

	util = sugov_iowait_apply(sg_cpu, time, util, max);
	sugov_calc_avg_cap(sg_policy, sg_cpu->walt_load.ws,
			   sg_policy->policy->cur);

	trace_sugov_util_update(sg_cpu->cpu, sg_cpu->util,
				sg_policy->avg_cap, max, sg_cpu->walt_load.nl,
				sg_cpu->walt_load.pl,
				sg_cpu->walt_load.rtgb_active, flags);

	sugov_walt_adjust(sg_cpu, &util, &max);
		next_f = get_next_freq(sg_policy, util, max, time);
	/*
	 * Do not reduce the frequency if the CPU has not been idle
	 * recently, as the reduction is likely to be premature then.
	 */
	if (busy && next_f < sg_policy->next_freq) {
		next_f = sg_policy->next_freq;

		/* Reset cached freq as next_freq has changed */
		sg_policy->cached_raw_freq = 0;
	}

	/*
	 * This code runs under rq->lock for the target CPU, so it won't run
	 * concurrently on two different CPUs for the same target and it is not
	 * necessary to acquire the lock in the fast switch case.
	 */
	if (sg_policy->policy->fast_switch_enabled) {
		sugov_fast_switch(sg_policy, time, next_f);
	} else {
		raw_spin_lock(&sg_policy->update_lock);
		sugov_deferred_update(sg_policy, time, next_f);
		raw_spin_unlock(&sg_policy->update_lock);
	}
}

static unsigned int sugov_next_freq_shared(struct sugov_cpu *sg_cpu, u64 time)
{
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	struct cpufreq_policy *policy = sg_policy->policy;
	u64 last_freq_update_time = sg_policy->last_freq_update_time;
	unsigned long util = 0, max = 1;
	unsigned int j;

	for_each_cpu(j, policy->cpus) {
		struct sugov_cpu *j_sg_cpu = &per_cpu(sugov_cpu, j);
		unsigned long j_util, j_max;
		s64 delta_ns;

		/*
		 * If the CPU utilization was last updated before the previous
		 * frequency update and the time elapsed between the last update
		 * of the CPU utilization and the last frequency update is long
		 * enough, don't take the CPU into account as it probably is
		 * idle now (and clear iowait_boost for it).
		 */
		delta_ns = last_freq_update_time - j_sg_cpu->last_update;
		if (delta_ns > stale_ns) {
			sugov_iowait_reset(j_sg_cpu, last_freq_update_time,
					   false);
			continue;
		}

		/*
		 * If the util value for all CPUs in a policy is 0, just using >
		 * will result in a max value of 1. WALT stats can later update
		 * the aggregated util value, causing get_next_freq() to compute
		 * freq = max_freq * 1.25 * (util / max) for nonzero util,
		 * leading to spurious jumps to fmax.
		 */
		j_util = j_sg_cpu->util;
		j_max = j_sg_cpu->max;
		j_util = sugov_iowait_apply(j_sg_cpu, time, j_util, j_max);

		if (j_util * max >= j_max * util) {
			util = j_util;
			max = j_max;
		}

		sugov_walt_adjust(j_sg_cpu, &util, &max);
	}

	return get_next_freq(sg_policy, util, max, time);
}

static void
sugov_update_shared(struct update_util_data *hook, u64 time, unsigned int flags)
{
	struct sugov_cpu *sg_cpu = container_of(hook, struct sugov_cpu, update_util);
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	unsigned long hs_util, boost_util;
	unsigned int next_f;

	if (!sg_policy->tunables->pl && flags & SCHED_CPUFREQ_PL)
		return;
	sg_policy->flags = flags;
	sg_cpu->util = sugov_get_util(sg_cpu);
	sg_cpu->flags = flags;
	raw_spin_lock(&sg_policy->update_lock);

	if (sg_policy->max != sg_cpu->max) {
		sg_policy->max = sg_cpu->max;
		hs_util = target_util(sg_policy,
					sg_policy->tunables->hispeed_freq);
		sg_policy->hispeed_util = hs_util;

		boost_util = target_util(sg_policy,
				    sg_policy->tunables->rtg_boost_freq);
		sg_policy->rtg_boost_util = boost_util;
	}

	sugov_iowait_boost(sg_cpu, time, flags);
	sg_cpu->last_update = time;

	sugov_calc_avg_cap(sg_policy, sg_cpu->walt_load.ws,
			   sg_policy->policy->cur);
	ignore_dl_rate_limit(sg_cpu, sg_policy);

	trace_sugov_util_update(sg_cpu->cpu, sg_cpu->util, sg_policy->avg_cap,
				sg_cpu->max, sg_cpu->walt_load.nl,
				sg_cpu->walt_load.pl,
				sg_cpu->walt_load.rtgb_active, flags);

	if (sugov_should_update_freq(sg_policy, time) &&
	    !(flags & SCHED_CPUFREQ_CONTINUE)) {
		next_f = sugov_next_freq_shared(sg_cpu, time);

		if (sg_policy->policy->fast_switch_enabled)
			sugov_fast_switch(sg_policy, time, next_f);
		else
			sugov_deferred_update(sg_policy, time, next_f);
	}

	raw_spin_unlock(&sg_policy->update_lock);
}

static void sugov_work(struct kthread_work *work)
{
	struct sugov_policy *sg_policy = container_of(work, struct sugov_policy, work);
	unsigned int freq;
	unsigned long flags;

	/*
	 * Hold sg_policy->update_lock shortly to handle the case where:
	 * incase sg_policy->next_freq is read here, and then updated by
	 * sugov_deferred_update() just before work_in_progress is set to false
	 * here, we may miss queueing the new update.
	 *
	 * Note: If a work was queued after the update_lock is released,
	 * sugov_work() will just be called again by kthread_work code; and the
	 * request will be proceed before the sugov thread sleeps.
	 */
	raw_spin_lock_irqsave(&sg_policy->update_lock, flags);
	freq = sg_policy->next_freq;
	if (use_pelt())
		sg_policy->work_in_progress = false;
	sugov_track_cycles(sg_policy, sg_policy->policy->cur,
			   ktime_get_ns());
	raw_spin_unlock_irqrestore(&sg_policy->update_lock, flags);

	mutex_lock(&sg_policy->work_lock);
	__cpufreq_driver_target(sg_policy->policy, freq, CPUFREQ_RELATION_L);
	mutex_unlock(&sg_policy->work_lock);
}

static void sugov_irq_work(struct irq_work *irq_work)
{
	struct sugov_policy *sg_policy;

	sg_policy = container_of(irq_work, struct sugov_policy, irq_work);

	kthread_queue_work(&sg_policy->worker, &sg_policy->work);
}

/************************** sysfs interface ************************/

static struct sugov_tunables *global_tunables;
static DEFINE_MUTEX(global_tunables_lock);

static inline struct sugov_tunables *to_sugov_tunables(struct gov_attr_set *attr_set)
{
	return container_of(attr_set, struct sugov_tunables, attr_set);
}

static DEFINE_MUTEX(min_rate_lock);

static void update_min_rate_limit_ns(struct sugov_policy *sg_policy)
{
	mutex_lock(&min_rate_lock);
	sg_policy->min_rate_limit_ns = min(sg_policy->up_rate_delay_ns,
					   sg_policy->down_rate_delay_ns);
	mutex_unlock(&min_rate_lock);
}

static ssize_t up_rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->up_rate_limit_us);
}

static ssize_t down_rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->down_rate_limit_us);
}

static ssize_t up_rate_limit_us_store(struct gov_attr_set *attr_set,
				      const char *buf, size_t count)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	struct sugov_policy *sg_policy;
	unsigned int rate_limit_us;

	if (kstrtouint(buf, 10, &rate_limit_us))
		return -EINVAL;

	tunables->up_rate_limit_us = rate_limit_us;

	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook) {
		sg_policy->up_rate_delay_ns = rate_limit_us * NSEC_PER_USEC;
		update_min_rate_limit_ns(sg_policy);
	}

	return count;
}

static ssize_t down_rate_limit_us_store(struct gov_attr_set *attr_set,
					const char *buf, size_t count)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	struct sugov_policy *sg_policy;
	unsigned int rate_limit_us;

	if (kstrtouint(buf, 10, &rate_limit_us))
		return -EINVAL;

	tunables->down_rate_limit_us = rate_limit_us;

	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook) {
		sg_policy->down_rate_delay_ns = rate_limit_us * NSEC_PER_USEC;
		update_min_rate_limit_ns(sg_policy);
	}

	return count;
}

static ssize_t hispeed_load_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->hispeed_load);
}

static ssize_t hispeed_load_store(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);

	if (kstrtouint(buf, 10, &tunables->hispeed_load))
		return -EINVAL;

	tunables->hispeed_load = min(100U, tunables->hispeed_load);

	return count;
}

static ssize_t hispeed_freq_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->hispeed_freq);
}

static ssize_t hispeed_freq_store(struct gov_attr_set *attr_set,
					const char *buf, size_t count)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	unsigned int val;
	struct sugov_policy *sg_policy;
	unsigned long hs_util;
	unsigned long flags;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	tunables->hispeed_freq = val;
	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook) {
		raw_spin_lock_irqsave(&sg_policy->update_lock, flags);
		hs_util = target_util(sg_policy,
					sg_policy->tunables->hispeed_freq);
		sg_policy->hispeed_util = hs_util;
		raw_spin_unlock_irqrestore(&sg_policy->update_lock, flags);
	}

	return count;
}

static ssize_t rtg_boost_freq_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->rtg_boost_freq);
}

static ssize_t rtg_boost_freq_store(struct gov_attr_set *attr_set,
				    const char *buf, size_t count)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	unsigned int val;
	struct sugov_policy *sg_policy;
	unsigned long boost_util;
	unsigned long flags;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	tunables->rtg_boost_freq = val;
	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook) {
		raw_spin_lock_irqsave(&sg_policy->update_lock, flags);
		boost_util = target_util(sg_policy,
					  sg_policy->tunables->rtg_boost_freq);
		sg_policy->rtg_boost_util = boost_util;
		raw_spin_unlock_irqrestore(&sg_policy->update_lock, flags);
	}

	return count;
}

static ssize_t efficient_freq_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->efficient_freq);
}
static ssize_t efficient_freq_store(struct gov_attr_set *attr_set,
				    const char *buf, size_t count)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	unsigned int val;
	if (kstrtouint(buf, 10, &val))
		return -EINVAL;
	tunables->efficient_freq = val;
	return count;
}
static ssize_t frame_aware_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->frame_aware);
}
static ssize_t frame_aware_store(struct gov_attr_set *attr_set, const char *buf,
				   size_t count)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	if (kstrtobool(buf, &tunables->frame_aware))
		return -EINVAL;
	return count;
}
static ssize_t pl_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->pl);
}

static ssize_t pl_store(struct gov_attr_set *attr_set, const char *buf,
				   size_t count)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);

	if (kstrtobool(buf, &tunables->pl))
		return -EINVAL;

	return count;
}

static struct governor_attr up_rate_limit_us = __ATTR_RW(up_rate_limit_us);
static struct governor_attr down_rate_limit_us = __ATTR_RW(down_rate_limit_us);
static struct governor_attr hispeed_load = __ATTR_RW(hispeed_load);
static struct governor_attr hispeed_freq = __ATTR_RW(hispeed_freq);
static struct governor_attr rtg_boost_freq = __ATTR_RW(rtg_boost_freq);
static struct governor_attr efficient_freq = __ATTR_RW(efficient_freq);
static struct governor_attr frame_aware = __ATTR_RW(frame_aware);
static struct governor_attr pl = __ATTR_RW(pl);

static struct attribute *sugov_attrs[] = {
	&up_rate_limit_us.attr,
	&down_rate_limit_us.attr,
	&hispeed_load.attr,
	&hispeed_freq.attr,
	&rtg_boost_freq.attr,
	&efficient_freq.attr,
	&frame_aware.attr,
	&pl.attr,
	NULL
};
ATTRIBUTE_GROUPS(sugov);

static void sugov_tunables_free(struct kobject *kobj)
{
	struct gov_attr_set *attr_set = container_of(kobj, struct gov_attr_set, kobj);

	kfree(to_sugov_tunables(attr_set));
}

static struct kobj_type sugov_tunables_ktype = {
	.default_groups = sugov_groups,
	.sysfs_ops = &governor_sysfs_ops,
	.release = &sugov_tunables_free,
};

/********************** cpufreq governor interface *********************/

static struct cpufreq_governor schedutil_gov;

static struct sugov_policy *sugov_policy_alloc(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy;

	sg_policy = kzalloc(sizeof(*sg_policy), GFP_KERNEL);
	if (!sg_policy)
		return NULL;

	sg_policy->policy = policy;
	raw_spin_lock_init(&sg_policy->update_lock);
	INIT_LIST_HEAD(&sg_policy->frame_boost_info);
	return sg_policy;
}

static void sugov_policy_free(struct sugov_policy *sg_policy)
{
	kfree(sg_policy);
}

static int sugov_kthread_create(struct sugov_policy *sg_policy)
{
	struct task_struct *thread;
	struct sched_param param = { .sched_priority = MAX_USER_RT_PRIO / 2 };
	struct cpufreq_policy *policy = sg_policy->policy;
	int ret;

	/* kthread only required for slow path */
	if (policy->fast_switch_enabled)
		return 0;

	kthread_init_work(&sg_policy->work, sugov_work);
	kthread_init_worker(&sg_policy->worker);
	thread = kthread_create(kthread_worker_fn, &sg_policy->worker,
				"sugov:%d",
				cpumask_first(policy->related_cpus));
	if (IS_ERR(thread)) {
		pr_err("failed to create sugov thread: %ld\n", PTR_ERR(thread));
		return PTR_ERR(thread);
	}

	ret = sched_setscheduler_nocheck(thread, SCHED_FIFO, &param);
	if (ret) {
		kthread_stop(thread);
		pr_warn("%s: failed to set SCHED_FIFO\n", __func__);
		return ret;
	}

	sg_policy->thread = thread;
	kthread_bind_mask(thread, policy->related_cpus);
	init_irq_work(&sg_policy->irq_work, sugov_irq_work);
	mutex_init(&sg_policy->work_lock);

	wake_up_process(thread);

	return 0;
}

static void sugov_kthread_stop(struct sugov_policy *sg_policy)
{
	/* kthread only required for slow path */
	if (sg_policy->policy->fast_switch_enabled)
		return;

	kthread_flush_worker(&sg_policy->worker);
	kthread_stop(sg_policy->thread);
	mutex_destroy(&sg_policy->work_lock);
}

static struct sugov_tunables *sugov_tunables_alloc(struct sugov_policy *sg_policy)
{
	struct sugov_tunables *tunables;

	tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);
	if (tunables) {
		gov_attr_set_init(&tunables->attr_set, &sg_policy->tunables_hook);
		if (!have_governor_per_policy())
			global_tunables = tunables;
	}
	return tunables;
}

static void sugov_tunables_save(struct cpufreq_policy *policy,
		struct sugov_tunables *tunables)
{
	int cpu;
	struct sugov_tunables *cached = per_cpu(cached_tunables, policy->cpu);

	if (!have_governor_per_policy())
		return;

	if (!cached) {
		cached = kzalloc(sizeof(*tunables), GFP_KERNEL);
		if (!cached)
			return;

		for_each_cpu(cpu, policy->related_cpus)
			per_cpu(cached_tunables, cpu) = cached;
	}

	cached->pl = tunables->pl;
	cached->hispeed_load = tunables->hispeed_load;
	cached->rtg_boost_freq = tunables->rtg_boost_freq;
	cached->efficient_freq = tunables->efficient_freq;
	cached->frame_aware = tunables->frame_aware;
	cached->hispeed_freq = tunables->hispeed_freq;
	cached->up_rate_limit_us = tunables->up_rate_limit_us;
	cached->down_rate_limit_us = tunables->down_rate_limit_us;
}

static void sugov_clear_global_tunables(void)
{
	if (!have_governor_per_policy())
		global_tunables = NULL;
}

static void sugov_tunables_restore(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	struct sugov_tunables *tunables = sg_policy->tunables;
	struct sugov_tunables *cached = per_cpu(cached_tunables, policy->cpu);

	if (!cached)
		return;

	tunables->pl = cached->pl;
	tunables->hispeed_load = cached->hispeed_load;
	tunables->rtg_boost_freq = cached->rtg_boost_freq;
	tunables->efficient_freq = cached->efficient_freq;
	tunables->frame_aware = cached->frame_aware;
	tunables->hispeed_freq = cached->hispeed_freq;
	tunables->up_rate_limit_us = cached->up_rate_limit_us;
	tunables->down_rate_limit_us = cached->down_rate_limit_us;
	sg_policy->up_rate_delay_ns = cached->up_rate_limit_us;
	sg_policy->down_rate_delay_ns = cached->down_rate_limit_us;
	update_min_rate_limit_ns(sg_policy);
}

static int sugov_init(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy;
	struct sugov_tunables *tunables;
	unsigned long util;
	int ret = 0;

	/* State should be equivalent to EXIT */
	if (policy->governor_data)
		return -EBUSY;

	cpufreq_enable_fast_switch(policy);

	sg_policy = sugov_policy_alloc(policy);
	if (!sg_policy) {
		ret = -ENOMEM;
		goto disable_fast_switch;
	}

	ret = sugov_kthread_create(sg_policy);
	if (ret)
		goto free_sg_policy;

	mutex_lock(&global_tunables_lock);

	if (global_tunables) {
		if (WARN_ON(have_governor_per_policy())) {
			ret = -EINVAL;
			goto stop_kthread;
		}
		policy->governor_data = sg_policy;
		sg_policy->tunables = global_tunables;

		gov_attr_set_get(&global_tunables->attr_set, &sg_policy->tunables_hook);
		goto out;
	}

	tunables = sugov_tunables_alloc(sg_policy);
	if (!tunables) {
		ret = -ENOMEM;
		goto stop_kthread;
	}

	tunables->up_rate_limit_us = cpufreq_policy_transition_delay_us(policy);
	tunables->down_rate_limit_us = cpufreq_policy_transition_delay_us(policy);
	tunables->hispeed_load = DEFAULT_HISPEED_LOAD;
	tunables->hispeed_freq = 0;

	switch (policy->cpu) {
	default:
	case 0:
		tunables->rtg_boost_freq = DEFAULT_CPU0_RTG_BOOST_FREQ;
		tunables->efficient_freq = DEFAULT_CPU0_EFFICIENT_FREQ;
		break;
	case 4:
		tunables->rtg_boost_freq = DEFAULT_CPU4_RTG_BOOST_FREQ;
		tunables->efficient_freq = DEFAULT_CPU4_EFFICIENT_FREQ;
		break;
	case 7:
		tunables->rtg_boost_freq = DEFAULT_CPU7_RTG_BOOST_FREQ;
		tunables->efficient_freq = DEFAULT_CPU7_EFFICIENT_FREQ;
		break;
	}
	tunables->frame_aware = true;
	policy->governor_data = sg_policy;
	sg_policy->tunables = tunables;

	util = target_util(sg_policy, sg_policy->tunables->rtg_boost_freq);
	sg_policy->rtg_boost_util = util;

	stale_ns = sched_ravg_window + (sched_ravg_window >> 3);

	sugov_tunables_restore(policy);

	ret = kobject_init_and_add(&tunables->attr_set.kobj, &sugov_tunables_ktype,
				   get_governor_parent_kobj(policy), "%s",
				   schedutil_gov.name);
	if (ret)
		goto fail;

out:
	mutex_unlock(&global_tunables_lock);
	return 0;

fail:
	kobject_put(&tunables->attr_set.kobj);
	policy->governor_data = NULL;
	sugov_clear_global_tunables();

stop_kthread:
	sugov_kthread_stop(sg_policy);
	mutex_unlock(&global_tunables_lock);

free_sg_policy:
	sugov_policy_free(sg_policy);

disable_fast_switch:
	cpufreq_disable_fast_switch(policy);

	pr_err("initialization failed (error %d)\n", ret);
	return ret;
}

static void sugov_exit(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	struct sugov_tunables *tunables = sg_policy->tunables;
	unsigned int count;

	mutex_lock(&global_tunables_lock);

	/* Save tunables before last owner release it in gov_attr_set_put() */
	if (tunables->attr_set.usage_count == 1)
		sugov_tunables_save(policy, tunables);

	count = gov_attr_set_put(&tunables->attr_set, &sg_policy->tunables_hook);
	policy->governor_data = NULL;
	if (!count)
		sugov_clear_global_tunables();

	mutex_unlock(&global_tunables_lock);

	sugov_kthread_stop(sg_policy);
	sugov_clean_frame_boost(sg_policy);
	sugov_policy_free(sg_policy);
	cpufreq_disable_fast_switch(policy);
}

static int sugov_start(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	unsigned int cpu;

	sg_policy->up_rate_delay_ns =
		sg_policy->tunables->up_rate_limit_us * NSEC_PER_USEC;
	sg_policy->down_rate_delay_ns =
		sg_policy->tunables->down_rate_limit_us * NSEC_PER_USEC;
	update_min_rate_limit_ns(sg_policy);
	sg_policy->last_freq_update_time	= 0;
	sg_policy->next_freq			= 0;
	sg_policy->work_in_progress		= false;
	sg_policy->limits_changed		= false;
	sg_policy->need_freq_update		= false;
	sg_policy->cached_raw_freq		= 0;

	for_each_cpu(cpu, policy->cpus) {
		struct sugov_cpu *sg_cpu = &per_cpu(sugov_cpu, cpu);

		memset(sg_cpu, 0, sizeof(*sg_cpu));
		sg_cpu->cpu			= cpu;
		sg_cpu->sg_policy		= sg_policy;
	}

	for_each_cpu(cpu, policy->cpus) {
		struct sugov_cpu *sg_cpu = &per_cpu(sugov_cpu, cpu);

		cpufreq_add_update_util_hook(cpu, &sg_cpu->update_util,
					     policy_is_shared(policy) ?
							sugov_update_shared :
							sugov_update_single);
	}
	return 0;
}

static void sugov_stop(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	unsigned int cpu;

	for_each_cpu(cpu, policy->cpus)
		cpufreq_remove_update_util_hook(cpu);

	synchronize_rcu();

	if (!policy->fast_switch_enabled) {
		irq_work_sync(&sg_policy->irq_work);
		kthread_cancel_work_sync(&sg_policy->work);
	}
	/**
	 * Clean it on stop so frametime handler knows that schedutil
	 * is not currently running on this cpu.
	 */
	for_each_cpu(cpu, policy->cpus) {
		struct sugov_cpu *sg_cpu = &per_cpu(sugov_cpu, cpu);
		memset(sg_cpu, 0, sizeof(*sg_cpu));
	}
}

static void sugov_limits(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	unsigned long flags, now;
	unsigned int freq;

	if (!policy->fast_switch_enabled) {
		mutex_lock(&sg_policy->work_lock);
		raw_spin_lock_irqsave(&sg_policy->update_lock, flags);
		sugov_track_cycles(sg_policy, sg_policy->policy->cur,
				   ktime_get_ns());
		raw_spin_unlock_irqrestore(&sg_policy->update_lock, flags);
		cpufreq_policy_apply_limits(policy);
		mutex_unlock(&sg_policy->work_lock);
	} else {
		raw_spin_lock_irqsave(&sg_policy->update_lock, flags);
		freq = policy->cur;
		now = ktime_get_ns();

		/*
		 * cpufreq_driver_resolve_freq() has a clamp, so we do not need
		 * to do any sort of additional validation here.
		 */
		freq = cpufreq_driver_resolve_freq(policy, freq);
		sg_policy->cached_raw_freq = freq;
		sugov_fast_switch(sg_policy, now, freq);
		raw_spin_unlock_irqrestore(&sg_policy->update_lock, flags);
	}

	sg_policy->limits_changed = true;
}

static struct cpufreq_governor schedutil_gov = {
	.name			= "schedutil",
	.owner			= THIS_MODULE,
	.dynamic_switching	= true,
	.init			= sugov_init,
	.exit			= sugov_exit,
	.start			= sugov_start,
	.stop			= sugov_stop,
	.limits			= sugov_limits,
};

static void sugov_calc_frame_boost(
        struct sugov_policy *sg_policy, struct frame_boost_info *info,
		unsigned int ui_frame_time, u64 cur_time)
{
	unsigned int freq;
	/**
	 * Evaluate the desired freq based on current freq and
	 * the last frame generation time. This may not be accurate
	 * because freq may change multiple times during the frame
	 * generation. But whatever, use this until finding a better
	 * way :)
	 */
	freq = mult_frac(sg_policy->policy->cur,
	        ui_frame_time, TARGET_FRAME_TIME);
	// Fallback to use the regular freq calculating method.
	if (freq <= sg_policy->tunables->efficient_freq)
		freq = 0;
	info->freq = freq;
	// Boost for next 10 frame time (120 Hz).
	info->expiration = cur_time + 10 * 8333 * NSEC_PER_USEC;
}
static void sugov_frametime_handler(
        unsigned int ui_frame_time, ktime_t cur_time)
{
	struct rq *rq;
	struct sugov_cpu *sg_cpu;
	struct frame_boost_info *info;
	int cpu;
	unsigned long flags;
	// Allocate objects when we are able to sleep.
	info = kmem_cache_alloc(kmem_frame_boost_pool, GFP_KERNEL);
	if (!info)
		return;
	cpu = get_cpu();
	rq = cpu_rq(cpu);
	sg_cpu = &per_cpu(sugov_cpu, cpu);
	// Schedutil is not running on this cpu now.
	if (sg_cpu->sg_policy == NULL ||
	        !sg_cpu->sg_policy->tunables->frame_aware) {
		kmem_cache_free(kmem_frame_boost_pool, info);
		goto out_put_cpu;
	}
	sugov_calc_frame_boost(sg_cpu->sg_policy, info, ui_frame_time, cur_time);
	raw_spin_lock_irqsave(&rq->lock, flags);
	sugov_push_frame_boost(sg_cpu->sg_policy, info);
	/*
	 * No need to call rcu_read_lock_sched() because it
	 * automatically get locked on disabling preemption.
	 */
	cpufreq_update_util(rq, SCHED_CPUFREQ_SKIP_LIMITS);
	raw_spin_unlock_irqrestore(&rq->lock, flags);
out_put_cpu:
	put_cpu();
}
static struct hwui_mon_receiver sugov_frametime_receiver = {
	.jank_frame_time = TARGET_FRAME_TIME,
	.jank_callback = sugov_frametime_handler
};

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDUTIL
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return &schedutil_gov;
}
#endif

static int __init sugov_init_frame_boost(void)
{
	kmem_frame_boost_pool = KMEM_CACHE(frame_boost_info,
	        SLAB_HWCACHE_ALIGN | SLAB_PANIC);
	return register_hwui_mon(&sugov_frametime_receiver);
}

static int __init sugov_register(void)
{
	return cpufreq_register_governor(&schedutil_gov) ||
	        sugov_init_frame_boost();

}
core_initcall(sugov_register);
