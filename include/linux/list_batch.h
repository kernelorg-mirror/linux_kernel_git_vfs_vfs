/*
 * List insertion/deletion batching facility
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * (C) Copyright 2016 Hewlett-Packard Enterprise Development LP
 *
 * Authors: Waiman Long <waiman.long@hpe.com>
 */
#ifndef __LINUX_LIST_BATCH_H
#define __LINUX_LIST_BATCH_H

#include <linux/spinlock.h>
#include <linux/list.h>

/*
 * include/linux/list_batch.h
 *
 * Inserting or deleting an entry from a linked list under a spinlock is a
 * very common operation in the Linux kernel. If many CPUs are trying to
 * grab the lock and manipulate the linked list, it can lead to significant
 * lock contention and slow operation.
 *
 * This list operation batching facility is used to batch multiple list
 * operations under one lock/unlock critical section, thus reducing the
 * locking overhead and improving overall performance.
 */
enum list_batch_cmd {
	lb_cmd_add,
	lb_cmd_del,
	lb_cmd_del_init
};

enum list_batch_state {
	lb_state_waiting,	/* Node is waiting */
	lb_state_batch,		/* Queue head to perform batch processing */
	lb_state_done		/* Job is done */
};

struct list_batch_qnode {
	struct list_batch_qnode	*next;
	struct list_head	*entry;
	enum list_batch_cmd	cmd;
	enum list_batch_state	state;
};

struct list_batch {
	struct list_head	*list;
	struct list_batch_qnode *tail;
};

#define LIST_BATCH_INIT(_list)	\
	{			\
		.list = _list,	\
		.tail = NULL	\
	}

static inline void list_batch_init(struct list_batch *batch,
				   struct list_head *list)
{
	batch->list = list;
	batch->tail = NULL;
}

static __always_inline void _list_batch_cmd(enum list_batch_cmd cmd,
					    struct list_head *head,
					    struct list_head *entry)
{
	if (cmd == lb_cmd_add)
		list_add(entry, head);
	else if (cmd == lb_cmd_del)
		list_del(entry);
	else /* cmd == lb_cmd_del_init */
		list_del_init(entry);
}

#ifdef CONFIG_LIST_BATCHING

extern void do_list_batch_slowpath(enum list_batch_cmd cmd, spinlock_t *lock,
				   struct list_batch *batch,
				   struct list_head *entry);

static inline void do_list_batch(enum list_batch_cmd cmd, spinlock_t *lock,
				   struct list_batch *batch,
				   struct list_head *entry)
{
	/*
	 * Fast path
	 */
	if (spin_trylock(lock)) {
		_list_batch_cmd(cmd, batch->list, entry);
		spin_unlock(lock);
		return;
	}
	do_list_batch_slowpath(cmd, lock, batch, entry);
}


#else /* CONFIG_LIST_BATCHING */

static inline void do_list_batch(enum list_batch_cmd cmd, spinlock_t *lock,
				   struct list_batch *batch,
				   struct list_head *entry)
{
	spin_lock(lock);
	_list_batch_cmd(cmd, batch->list, entry);
	spin_unlock(lock);
}

#endif /* CONFIG_LIST_BATCHING */

#endif /* __LINUX_LIST_BATCH_H */
