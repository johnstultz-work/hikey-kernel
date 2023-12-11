// SPDX-License-Identifier: GPL-2.0+
/*
 * Module-based test case for RT scheduling invariant
 *
 * A reimplementation of my old sched_football test
 * found in LTP:
 *   https://github.com/linux-test-project/ltp/blob/master/testcases/realtime/func/sched_football/sched_football.c
 *
 * Similar to that test, this tries to validate the RT
 * scheduling invariant, that the across N available cpus, the
 * top N priority tasks always running.
 *
 * This is done via having N offsensive players that are
 * medium priority, which constantly are trying to increment the
 * ball_pos counter.
 *
 * Blocking this, are N defensive players that are higher
 * priority which just spin on the cpu, preventing the medium
 * priroity tasks from running.
 *
 * To complicate this, there are also N defensive low priority
 * tasks. These start first and each aquire one of N mutexes.
 * The high priority defense tasks will later try to grab the
 * mutexes and block, opening a window for the offsensive tasks
 * to run and increment the ball. If priority inheritance or
 * proxy execution is used, the low priority defense players
 * should be boosted to the high priority levels, and will
 * prevent the mid priority offensive tasks from running.
 *
 * Copyright © International Business Machines  Corp., 2007, 2008
 * Copyright (C) Google, 2023
 *
 * Authors: John Stultz <jstultz@google.com>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/sched/rt.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/smp.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <uapi/linux/sched/types.h>
#include <linux/rtmutex.h>

atomic_t players_ready;
atomic_t ball_pos;
int players_per_team;
bool game_over;

struct mutex *mutex_low_list;
struct mutex *mutex_mid_list;

static inline
struct task_struct *create_fifo_thread(int (*threadfn)(void *data), void *data,
				       char *name, int prio)
{
	struct task_struct *kth;
	struct sched_attr attr = {
		.size		= sizeof(struct sched_attr),
		.sched_policy	= SCHED_FIFO,
		.sched_nice	= 0,
		.sched_priority	= prio,
	};
	int ret;

	kth = kthread_create(threadfn, data, name);
	if (IS_ERR(kth)) {
		pr_warn("%s eerr, kthread_create failed\n", __func__);
		return kth;
	}
	ret = sched_setattr_nocheck(kth, &attr);
	if (ret) {
		kthread_stop(kth);
		pr_warn("%s: failed to set SCHED_FIFO\n", __func__);
		return ERR_PTR(ret);
	}

	wake_up_process(kth);
	return kth;
}

int defense_low_thread(void *arg)
{
	long tnum = (long)arg;

	atomic_inc(&players_ready);
	mutex_lock(&mutex_low_list[tnum]);
	while (!READ_ONCE(game_over)) {
		if (kthread_should_stop())
			break;
		schedule();
	}
	mutex_unlock(&mutex_low_list[tnum]);
	return 0;
}

int defense_mid_thread(void *arg)
{
	long tnum = (long)arg;

	atomic_inc(&players_ready);
	mutex_lock(&mutex_mid_list[tnum]);
	mutex_lock(&mutex_low_list[tnum]);
	while (!READ_ONCE(game_over)) {
		if (kthread_should_stop())
			break;
		schedule();
	}
	mutex_unlock(&mutex_low_list[tnum]);
	mutex_unlock(&mutex_mid_list[tnum]);
	return 0;
}

int offense_thread(void *)
{
	atomic_inc(&players_ready);
	while (!READ_ONCE(game_over)) {
		if (kthread_should_stop())
			break;
		schedule();
		atomic_inc(&ball_pos);
	}
	return 0;
}

int defense_hi_thread(void *arg)
{
	long tnum = (long)arg;

	atomic_inc(&players_ready);
	mutex_lock(&mutex_mid_list[tnum]);
	while (!READ_ONCE(game_over)) {
		if (kthread_should_stop())
			break;
		schedule();
	}
	mutex_unlock(&mutex_mid_list[tnum]);
	return 0;
}

int crazy_fan_thread(void *)
{
	int count = 0;

	atomic_inc(&players_ready);
	while (!READ_ONCE(game_over)) {
		if (kthread_should_stop())
			break;
		schedule();
		udelay(1000);
		msleep(2);
		count++;
	}
	return 0;
}

int ref_thread(void *arg)
{
	struct task_struct *kth;
	long game_time = (long)arg;
	unsigned long final_pos;
	int i;

	pr_info("%s: started ref, game_time: %ld secs !\n", __func__,
		game_time);

	/* Create low  priority defensive team */
	for (i = 0; i < players_per_team; i++)
		kth = create_fifo_thread(defense_low_thread, (void *)i,
					 "defese-low-thread", 2);
	/* Wait for the defense threads to start */
	while (atomic_read(&players_ready) < players_per_team)
		msleep(1);

	for (i = 0; i < players_per_team; i++)
		kth = create_fifo_thread(defense_mid_thread,
					 (void *)(players_per_team - i - 1),
					 "defese-mid-thread", 3);
	/* Wait for the defense threads to start */
	while (atomic_read(&players_ready) < players_per_team * 2)
		msleep(1);

	/* Create mid priority offensive team */
	for (i = 0; i < players_per_team; i++)
		kth = create_fifo_thread(offense_thread, NULL,
					 "offense-thread", 5);
	/* Wait for the offense threads to start */
	while (atomic_read(&players_ready) < players_per_team * 3)
		msleep(1);

	/* Create high priority defensive team */
	for (i = 0; i < players_per_team; i++)
		kth = create_fifo_thread(defense_hi_thread, (void *)i,
					 "defese-hi-thread", 10);
	/* Wait for the defense threads to start */
	while (atomic_read(&players_ready) < players_per_team * 4)
		msleep(1);

	/* Create high priority defensive team */
	for (i = 0; i < players_per_team; i++)
		kth = create_fifo_thread(crazy_fan_thread, NULL,
					 "crazy-fan-thread", 15);
	/* Wait for the defense threads to start */
	while (atomic_read(&players_ready) < players_per_team * 5)
		msleep(1);

	pr_info("%s: all players checked in! Starting game.\n", __func__);
	atomic_set(&ball_pos, 0);
	msleep(game_time * 1000);
	final_pos = atomic_read(&ball_pos);
	pr_info("%s: final ball_pos: %ld\n", __func__, final_pos);
	WARN_ON(final_pos != 0);
	game_over = true;
	return 0;
}

static int __init test_sched_football_init(void)
{
	struct task_struct *kth;
	int i;

	players_per_team = num_online_cpus();

	mutex_low_list = kmalloc_array(players_per_team,  sizeof(struct mutex), GFP_ATOMIC);
	mutex_mid_list = kmalloc_array(players_per_team,  sizeof(struct mutex), GFP_ATOMIC);

	for (i = 0; i < players_per_team; i++) {
		mutex_init(&mutex_low_list[i]);
		mutex_init(&mutex_mid_list[i]);
	}

	kth = create_fifo_thread(ref_thread, (void *)10, "ref-thread", 20);

	return 0;
}
module_init(test_sched_football_init);
