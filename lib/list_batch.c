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
#include <linux/list_batch.h>

/*
 * List processing batch size = 128
 *
 * The batch size shouldn't be too large. Otherwise, it will be too unfair
 * to the task doing the batch processing. It shouldn't be too small neither
 * as the performance benefit will be reduced.
 */
#define LB_BATCH_SIZE	(1 << 7)

/*
 * Inserting or deleting an entry from a linked list under a spinlock is a
 * very common operation in the Linux kernel. If many CPUs are trying to
 * grab the lock and manipulate the linked list, it can lead to significant
 * lock contention and slow operation.
 *
 * This list operation batching facility is used to batch multiple list
 * operations under one lock/unlock critical section, thus reducing the
 * locking overhead and improving overall performance.
 */
void do_list_batch_slowpath(enum list_batch_cmd cmd, spinlock_t *lock,
			    struct list_batch *batch, struct list_head *entry)
{
	struct list_batch_qnode node, *prev, *next, *nptr;
	int loop;

	/*
	 * Put itself into the list_batch queue
	 */
	node.next  = NULL;
	node.entry = entry;
	node.cmd   = cmd;
	node.state = lb_state_waiting;

	prev = xchg(&batch->tail, &node);

	if (prev) {
		WRITE_ONCE(prev->next, &node);
		while (READ_ONCE(node.state) == lb_state_waiting)
			cpu_relax();
		if (node.state == lb_state_done)
			return;
		WARN_ON(node.state != lb_state_batch);
	}

	/*
	 * We are now the queue head, we shold now acquire the lock and
	 * process a batch of qnodes.
	 */
	loop = LB_BATCH_SIZE;
	next = &node;
	spin_lock(lock);

do_list_again:
	do {
		nptr = next;
		_list_batch_cmd(nptr->cmd, batch->list, nptr->entry);
		next = READ_ONCE(nptr->next);
		/*
		 * As soon as the state is marked lb_state_done, we
		 * can no longer assume the content of *nptr as valid.
		 * So we have to hold off marking it done until we no
		 * longer need its content.
		 *
		 * The release barrier here is to make sure that we
		 * won't access its content after marking it done.
		 */
		if (next)
			smp_store_release(&nptr->state, lb_state_done);
	} while (--loop && next);
	if (!next) {
		/*
		 * The queue tail should equal to nptr, so clear it to
		 * mark the queue as empty.
		 */
		if (cmpxchg(&batch->tail, nptr, NULL) != nptr) {
			/*
			 * Queue not empty, wait until the next pointer is
			 * initialized.
			 */
			while (!(next = READ_ONCE(nptr->next)))
				cpu_relax();
		}
		/* The above cmpxchg acts as a memory barrier */
		WRITE_ONCE(nptr->state, lb_state_done);
	}
	if (next) {
		if (loop)
			goto do_list_again;	/* More qnodes to process */
		/*
		 * Mark the next qnode as head to process the next batch
		 * of qnodes. The new queue head cannot proceed until we
		 * release the lock.
		 */
		WRITE_ONCE(next->state, lb_state_batch);
	}
	spin_unlock(lock);
}
EXPORT_SYMBOL_GPL(do_list_batch_slowpath);
