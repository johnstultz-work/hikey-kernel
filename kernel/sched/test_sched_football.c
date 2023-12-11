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
 * This is done via having N offensive players that are
 * medium priority, which constantly are trying to increment the
 * ball_pos counter.
 *
 * Blocking this are N defensive players that are higher
 * priority which just spin on the cpu, preventing the medium
 * priority tasks from running.
 *
 * To complicate this, there are also N defensive low priority
 * tasks. These start first and each acquire one of N mutexes.
 * The high priority defense tasks will later try to grab the
 * mutexes and block, opening a window for the offensive tasks
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

#define MODULE_NAME "sched_football"
#define pr_fmt(fmt) MODULE_NAME ": " fmt

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

MODULE_AUTHOR("John Stultz <jstultz@google.com>");
MODULE_DESCRIPTION("Test case for RT scheduling invariant");
MODULE_LICENSE("GPL");

atomic_t players_ready;
atomic_t ball_pos;
unsigned int players_per_team;
bool game_over;

#if CONFIG_SCHED_PROXY_EXEC
struct mutex *mutex_low_list;
struct mutex *mutex_mid_list;
#define test_lock_init(x)	mutex_init(x)
#define TEST_LOCK_SIZE		sizeof(struct mutex)
#define test_lock(x)		mutex_lock(x)
#define test_unlock(x)		mutex_unlock(x)
#else
struct rt_mutex *mutex_low_list;
struct rt_mutex *mutex_mid_list;
#define test_lock_init(x)	rt_mutex_init(x)
#define TEST_LOCK_SIZE		sizeof(struct rt_mutex)
#define test_lock(x)		rt_mutex_lock(x)
#define test_unlock(x)		rt_mutex_unlock(x)
#endif

static struct task_struct *create_fifo_thread(int (*threadfn)(void *data),
					      void *data, char *name, int prio)
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
		pr_warn("%s: Error, kthread_create failed\n", __func__);
		return kth;
	}
	ret = sched_setattr_nocheck(kth, &attr);
	if (ret) {
		kthread_stop(kth);
		pr_warn("%s: Error, failed to set SCHED_FIFO\n", __func__);
		return ERR_PTR(ret);
	}

	wake_up_process(kth);
	return kth;
}

static int spawn_players(int (*threadfn)(void *data), char *name, int prio)
{
	int current_players = atomic_read(&players_ready);
	struct task_struct *kth;
	long i;
	long start;

	/* Create players_per_team threads */
	for (i = 0; i < players_per_team; i++) {
		kth = create_fifo_thread(threadfn, (void *)i, name, prio);
		if (IS_ERR(kth))
			return -1;
	}

	start = jiffies;
	/* Wait for players_per_team threads to check in */
	while (atomic_read(&players_ready) < current_players + players_per_team) {
		msleep(1);
		if (jiffies - start > 30 * HZ) {
			pr_err("%s: Error, %s players took too long to checkin "
			       "(only %i of %i checked in)\n", __func__, name,
			       atomic_read(&players_ready),
			       current_players + players_per_team);
			return -1;
		}
	}
	return 0;
}

static int defense_low_thread(void *arg)
{
	long tnum = (long)arg;

	atomic_inc(&players_ready);
	test_lock(&mutex_low_list[tnum]);
	while (!READ_ONCE(game_over)) {
		if (kthread_should_stop())
			break;
		schedule();
	}
	test_unlock(&mutex_low_list[tnum]);
	return 0;
}

static int defense_mid_thread(void *arg)
{
	long tnum = (long)arg;

	atomic_inc(&players_ready);
	test_lock(&mutex_mid_list[tnum]);
	test_lock(&mutex_low_list[tnum]);
	while (!READ_ONCE(game_over)) {
		if (kthread_should_stop())
			break;
		schedule();
	}
	test_unlock(&mutex_low_list[tnum]);
	test_unlock(&mutex_mid_list[tnum]);
	return 0;
}

static int offense_thread(void *arg)
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

static int defense_hi_thread(void *arg)
{
	long tnum = (long)arg;

	atomic_inc(&players_ready);
	test_lock(&mutex_mid_list[tnum]);
	while (!READ_ONCE(game_over)) {
		if (kthread_should_stop())
			break;
		schedule();
	}
	test_unlock(&mutex_mid_list[tnum]);
	return 0;
}

static int crazy_fan_thread(void *arg)
{
	atomic_inc(&players_ready);
	while (!READ_ONCE(game_over)) {
		if (kthread_should_stop())
			break;
		schedule();
		udelay(1000);
		msleep(2);
	}
	return 0;
}

static int referee_thread(void *arg)
{
	long game_time = (long)arg;
	unsigned long final_pos;

	pr_info("Started referee, game_time: %ld secs !\n", game_time);
	/* Create low  priority defensive team */
	if (spawn_players(defense_low_thread, "defense-low-thread", 2))
		goto out;

	if (spawn_players(defense_mid_thread, "defense-mid-thread", 3))
		goto out;

	/* Create mid priority offensive team */
	if (spawn_players(offense_thread, "offense-thread", 5))
		goto out;

	/* Create high priority defensive team */
	if (spawn_players(defense_hi_thread, "defense-hi-thread", 10))
		goto out;

	/* Create high priority crazy fan threads */
	if (spawn_players(crazy_fan_thread, "crazy-fan-thread", 15))
		goto out;
	pr_info("All players checked in! Starting game.\n");
	atomic_set(&ball_pos, 0);
	msleep(game_time * 1000);
	final_pos = atomic_read(&ball_pos);
	WRITE_ONCE(game_over, true);
	pr_info("Final ball_pos: %ld\n",  final_pos);
	WARN_ON(final_pos != 0);
out:
	pr_info("Game Over!\n");
	WRITE_ONCE(game_over, true);
	return 0;
}

static int __init test_sched_football_init(void)
{
	struct task_struct *kth;
	int i;

	players_per_team = num_online_cpus();

	mutex_low_list = kmalloc_array(players_per_team, TEST_LOCK_SIZE, GFP_ATOMIC);
	mutex_mid_list = kmalloc_array(players_per_team, TEST_LOCK_SIZE, GFP_ATOMIC);
	if (!mutex_low_list || !mutex_mid_list)
		return -1;

	for (i = 0; i < players_per_team; i++) {
		test_lock_init(&mutex_low_list[i]);
		test_lock_init(&mutex_mid_list[i]);
	}

	kth = create_fifo_thread(referee_thread, (void *)10, "referee-thread", 20);
	if (IS_ERR(kth))
		return -1;
	return 0;
}
module_init(test_sched_football_init);
