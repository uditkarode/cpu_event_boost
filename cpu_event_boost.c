/*
 * SPDX-License-Identifier: Apache License 2.0
 * Copyright (C) 2020 Udit Karode <udit.karode@gmail.com>
 */

 //temp
#define CONFIG_COMPENSATE_BOOST_DURATION_MS 1234
#define CONFIG_COMPENSATE_MID_BOOST_FREQ_LP 1234
#define CONFIG_COMPENSATE_CRIT_BOOST_FREQ_LP 1234
#define CONFIG_COMPENSATE_MID_BOOST_FREQ_PERF 1234
#define CONFIG_COMPENSATE_CRIT_BOOST_FREQ_PERF 1234
#define CONFIG_CEB_COMPENSATE_MED_LIMIT 1234
#define CONFIG_CEB_COMPENSATE_CRIT_LIMIT 1234
 //temp

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/kthread.h>
#include <linux/msm_drm_notify.h>
#include <uapi/linux/sched/types.h>

#ifdef pr_fmt
#undef pr_fmt
#endif
#define pr_fmt(fmt) "cpu_event_boost: [%s:%d]: " fmt, __func__, __LINE__

static DECLARE_WAIT_QUEUE_HEAD(boost_waitq);
static DEFINE_KTHREAD_WORKER(event_boost_worker);

enum options {
	SCREEN_ON,
	MID_CPST_BOOST_ACTIVE,
	CRIT_CPST_BOOST_ACTIVE,
	MAX_CPU_BOOST_ACTIVE
};

struct event_boost {
	struct kthread_delayed_work unboost_work;
	struct notifier_block msm_drm_notif;
	struct notifier_block cpu_notif;
	atomic_long_t max_boost_expires;
	unsigned long options;
};

static struct event_boost event_boost_obj __read_mostly;

static void unboost_worker() {
	if(test_bit(MID_CPST_BOOST_ACTIVE, &event_boost_obj.options))
		clear_bit(MID_CPST_BOOST_ACTIVE, &event_boost_obj.options);
	if(test_bit(CRIT_CPST_BOOST_ACTIVE, &event_boost_obj.options))
		clear_bit(CRIT_CPST_BOOST_ACTIVE, &event_boost_obj.options);
	if(test_bit(MAX_CPU_BOOST_ACTIVE, &event_boost_obj.options))
		clear_bit(MAX_CPU_BOOST_ACTIVE, &event_boost_obj.options);
	wake_up(&boost_waitq);
}

static unsigned int get_compensate_boost_freq(struct cpufreq_policy *policy, enum options op)
{
	unsigned int freq;

	if (cpumask_test_cpu(policy->cpu, cpu_lp_mask)) {
		if (op == MID_CPST_BOOST_ACTIVE) {
			freq = max(CONFIG_COMPENSATE_MID_BOOST_FREQ_LP, CONFIG_MIN_FREQ_LP);
		} else if (op == CRIT_CPST_BOOST_ACTIVE) {
			freq = max(CONFIG_COMPENSATE_CRIT_BOOST_FREQ_LP, CONFIG_MIN_FREQ_LP);
		}
	} else {
		if (op == MID_CPST_BOOST_ACTIVE) {
			freq = max(CONFIG_COMPENSATE_MID_BOOST_FREQ_PERF, CONFIG_MIN_FREQ_LP);
		} else if (op == CRIT_CPST_BOOST_ACTIVE) {
			freq = max(CONFIG_COMPENSATE_CRIT_BOOST_FREQ_PERF, CONFIG_MIN_FREQ_LP);
		}
	}

	return min(freq, policy->max);
}

static unsigned int get_max_boost_freq(struct cpufreq_policy *policy)
{
	unsigned int freq;

	if (cpumask_test_cpu(policy->cpu, cpu_lp_mask))
		freq = CONFIG_MAX_BOOST_FREQ_LP;
	else
		freq = CONFIG_MAX_BOOST_FREQ_PERF;

	return min(freq, policy->max);
}

static void update_online_cpu_policy(void)
{
	unsigned int cpu;

	/* Only one CPU from each cluster needs to be updated */
	get_online_cpus();
	cpu = cpumask_first_and(cpu_lp_mask, cpu_online_mask);
	cpufreq_update_policy(cpu);
	cpu = cpumask_first_and(cpu_perf_mask, cpu_online_mask);
	cpufreq_update_policy(cpu);
	put_online_cpus();
}

static void cpu_compensate_boost_kick(unsigned long fb_delay)
{
	if (!test_bit(SCREEN_ON, &event_boost_obj.options))
		return;

	if(fb_delay > CONFIG_CEB_COMPENSATE_CRIT_LIMIT)
		set_bit(MID_CPST_BOOST_ACTIVE, &event_boost_obj.options);
	else if(fb_delay > CONFIG_CEB_COMPENSATE_MED_LIMIT)
		set_bit(CRIT_CPST_BOOST_ACTIVE, &event_boost_obj.options);
	
	if (!kthread_mod_delayed_work(&event_boost_worker, &event_boost_obj.unboost_work,
			      msecs_to_jiffies(CONFIG_COMPENSATE_BOOST_DURATION_MS)))
		wake_up(&boost_waitq);
}

static void cpu_event_boost_max(unsigned int duration_ms)
{
	unsigned long boost_jiffies = msecs_to_jiffies(duration_ms);
	unsigned long curr_expires, new_expires;

	if (!test_bit(SCREEN_ON, &event_boost_obj.options))
		return;

	curr_expires = atomic_long_read(&event_boost_obj.max_boost_expires);
	new_expires = jiffies + boost_jiffies;

	/* Skip this boost if there's a longer boost in effect */
	if (time_after(curr_expires, new_expires))
		return;
	
	atomic_long_cmpxchg(&event_boost_obj.max_boost_expires, curr_expires,
				     new_expires);

	set_bit(MAX_CPU_BOOST_ACTIVE, &event_boost_obj.options);
	if (!kthread_mod_delayed_work(&event_boost_worker, &event_boost_obj.unboost_work,
			      boost_jiffies))
		wake_up(&boost_waitq);
}

static int cpu_boost_thread()
{
	static const struct sched_param event_boost_rt_prio = {
		.sched_priority = MAX_RT_PRIO - 1
	};
	
	unsigned long old_state = -1;

	sched_setscheduler_nocheck(current, SCHED_RR, &event_boost_rt_prio);

	while (1) {
		bool should_stop = false;
		unsigned long curr_state;

		wait_event(boost_waitq,
			(curr_state = READ_ONCE(event_boost_obj.options)) != old_state ||
			(should_stop = kthread_should_stop()));

		if (should_stop)
			break;

		old_state = curr_state;
		update_online_cpu_policy();
	}

	return 0;
}

static int cpu_notifier_cb(struct notifier_block *nb, unsigned long action,
			   void *data)
{
	struct cpufreq_policy *policy = data;

	if (action != CPUFREQ_ADJUST)
		return NOTIFY_OK;

	if (!test_bit(SCREEN_ON, &event_boost_obj.options)) {
		policy->min = policy->cpuinfo.min_freq;
		return NOTIFY_OK;
	}

	if (test_bit(MAX_CPU_BOOST_ACTIVE, &event_boost_obj.options)) {
		policy->min = get_max_boost_freq(policy);
		return NOTIFY_OK;
	}

	if (test_bit(MID_CPST_BOOST_ACTIVE, &event_boost_obj.options))
		policy->min = get_compensate_boost_freq(policy, MID_CPST_BOOST_ACTIVE);
	else if (test_bit(CRIT_CPST_BOOST_ACTIVE, &event_boost_obj.options))
		policy->min = get_compensate_boost_freq(policy, CRIT_CPST_BOOST_ACTIVE);
	else if (cpumask_test_cpu(policy->cpu, cpu_lp_mask))
		policy->min = CONFIG_MIN_FREQ_LP;
	else
		policy->min = CONFIG_MIN_FREQ_PERF;

	return NOTIFY_OK;
}

static int msm_drm_notifier_cb(struct notifier_block *nb, unsigned long action,
			  void *data)
{
	struct msm_drm_notifier *evdata = data;
	int *blank = evdata->data;

	/* Parse framebuffer blank events as soon as they occur */
	if (action != MSM_DRM_EARLY_EVENT_BLANK)
		return NOTIFY_OK;

	if (*blank == MSM_DRM_BLANK_UNBLANK) {
		set_bit(SCREEN_ON, &event_boost_obj.options);
	} else {
		clear_bit(SCREEN_ON, &event_boost_obj.options);
		unboost_worker();
	}

	return NOTIFY_OK;
}

static int __init cpu_event_boost_init(void) {
	struct task_struct *thread;
	int ret;

	kthread_init_delayed_work(&event_boost_obj.unboost_work, &unboost_worker);
	init_waitqueue_head(&boost_waitq);

	event_boost_obj.cpu_notif.notifier_call = cpu_notifier_cb;
	ret = cpufreq_register_notifier(&event_boost_obj.cpu_notif, CPUFREQ_POLICY_NOTIFIER);
	if (ret) {
		pr_err("Failed to register cpufreq notifier, err: %d\n", ret);
		return ret;
	}

	event_boost_obj.msm_drm_notif.notifier_call = msm_drm_notifier_cb;
	event_boost_obj.msm_drm_notif.priority = INT_MAX;
	ret = msm_drm_register_client(&event_boost_obj.msm_drm_notif);
	if (ret) {
		pr_err("Failed to register msm_drm notifier, err: %d\n", ret);
		return ret;
	}

	thread = kthread_run(cpu_boost_thread, &event_boost_obj, "cpu_boostd");
	if (IS_ERR(thread)) {
		ret = PTR_ERR(thread);
		pr_err("Failed to start CPU boost thread, err: %d\n", ret);
		return ret;
	}

	return 0;
}

subsys_initcall(cpu_event_boost_init);